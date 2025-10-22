#include <iostream>
#include <atomic>
#include <chrono>
#include <unistd.h>
#include <signal.h>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <vector>
#include <asio.hpp>
#include <optional>

#include "k_datafifo.h"
#include "k_ipcmsg.h"
#include "../../driver_assistant_detector/common.h"
#include "media_streamer_file.h"
#include "media_streamer_rtsp.h"

// datafifo
#define READER_INDEX    0
#define WRITER_INDEX    1
static k_datafifo_handle hDataFifo[2] = {
    (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE, (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE
};

std::atomic<bool> send_stop(false);

std::vector<DetectionNormalizedCommon>  pending_detections;
uint64_t pending_detections_pts = UINT64_MAX;
std::mutex pending_detections_mutex;

using namespace std::chrono_literals;

std::mutex stream_endpoint_mutex;
asio::ip::udp::endpoint stream_endpoint_detections;

struct BbFiles_t {
    std::string video;
    std::string log;
};

static void release(void *pStream) {
    //printf("release %p\n", pStream);
}

int datafifo_init(k_u64 reader_phyAddr, k_u64 writer_phyAddr) {
    k_s32 s32Ret = K_SUCCESS;
    k_datafifo_params_s params_reader = {10, DATAFIFO_DETECTOR_BLOCK_LEN, K_TRUE, DATAFIFO_READER};
    s32Ret = kd_datafifo_open_by_addr(&hDataFifo[READER_INDEX], &params_reader, reader_phyAddr);
    if (K_SUCCESS != s32Ret) {
        printf("open datafifo error:%x\n", s32Ret);
        return -1;
    }

    k_datafifo_params_s params_writer = {2, DATAFIFO_FRONT_BLOCK_LEN, K_TRUE, DATAFIFO_WRITER};
    s32Ret = kd_datafifo_open_by_addr(&hDataFifo[WRITER_INDEX], &params_writer, writer_phyAddr);
    if (K_SUCCESS != s32Ret)
    {
        printf("open datafifo error:%x\n", s32Ret);
        return -1;
    }

    s32Ret = kd_datafifo_cmd(hDataFifo[WRITER_INDEX], DATAFIFO_CMD_SET_DATA_RELEASE_CALLBACK, (void *) release);
    if (K_SUCCESS != s32Ret)
    {
        printf("set release func callback error:%x\n", s32Ret);
        return -1;
    }

    printf("datafifo_init finish\n");

    return 0;
}

void datafifo_deinit() {
    k_s32 s32Ret = K_SUCCESS;
    // call write NULL to flush and release stream buffer.
    s32Ret = kd_datafifo_write(hDataFifo[WRITER_INDEX], NULL);
    if (K_SUCCESS != s32Ret)
    {
        printf("write error:%x\n", s32Ret);
    }

    kd_datafifo_close(hDataFifo[READER_INDEX]);
    kd_datafifo_close(hDataFifo[WRITER_INDEX]);

    printf("datafifo_deinit finish\n");
}

static void Usage() {
    std::cout << "Usage: ./driver_assistant_front [-p phyAddr] [-b bb_path] [-d]" << std::endl;
    std::cout << "-p: phyAddr (physical address for datafifo)" << std::endl;
    std::cout << "-b: bb_path (path for output files)" << std::endl;
    std::cout << "-d: daemon mode" << std::endl;
    exit(-1);
}

int parse_config(int argc, char *argv[], std::optional<std::string> &bb, bool &daemon_mode) {
    daemon_mode = false;

    int result;
    opterr = 0;
    while ((result = getopt(argc, argv, "H:b:d")) != -1) {
        switch (result) {
            case 'H': {
                Usage();
                break;
            }
            case 'b': {
                bb = std::make_optional(optarg);
                break;
            }
            case 'd': {
                daemon_mode = true;
                break;
            }
            default: Usage();
                break;
        }
    }
    return 0;
}

void read_fifo(asio::ip::udp::socket *udp_socket, k_s32 ipcmsg_handle, const std::optional<BbFiles_t> &bb_files_path) {
    k_u32 readLen = 0;
    k_s32 s32Ret = K_SUCCESS;
    int counter = 0;

    MediaStreamerFile streamer_file;
    FILE *output_file_detections = nullptr;
    if (bb_files_path.has_value()) {
        streamer_file.init(bb_files_path.value().video.c_str(), 1920, 1080);
        output_file_detections = fopen(bb_files_path.value().log.c_str(), "w");
    }

    MediaStreamerRtsp streamer_rtsp;
    streamer_rtsp.init("live", 1920, 1080);

    bool recording_started = false;
    std::vector<uint8_t> header_buffer;
    uint64_t first_frame_time_stamp = UINT64_MAX;

    char common_buf[1024*10];

    while (!send_stop) {
        readLen = 0;
        s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_GET_AVAIL_READ_LEN, &readLen);
        if (K_SUCCESS != s32Ret) {
            printf("get available read len error:%x\n", s32Ret);
            break;
        }

        if (readLen > 0) {
            k_char *pBuf;
            s32Ret = kd_datafifo_read(hDataFifo[READER_INDEX], reinterpret_cast<void **>(&pBuf));
            if (K_SUCCESS != s32Ret) {
                printf("read error:%x\n", s32Ret);
                break;
            }

            auto frame = reinterpret_cast<DataFifoFrame_t*>(pBuf);

            // FIX: Only start timestamp normalization from first real video frame (type 2), not header (type 3)
            // Header is generated at init time, but first video frame comes much later
            if (first_frame_time_stamp == UINT64_MAX && frame->type == 2) {
                first_frame_time_stamp = frame->pts;
            }
            const auto pts = (first_frame_time_stamp != UINT64_MAX) ? (frame->pts - first_frame_time_stamp) : frame->pts;

            uint64_t microseconds = pts;
            uint64_t milliseconds = microseconds / 1000;
            uint64_t seconds = milliseconds / 1000;
            uint64_t minutes = seconds / 60;
            uint64_t hours = minutes / 60;
            printf("Read frame. pts: %lu %02lu:%02lu:%02lu.%03lu, type %d, len %u\n", pts,
                   hours, minutes % 60, seconds % 60, milliseconds % 1000, frame->type, frame->data_len);

            if (!recording_started) {
                if (frame->type == 3) {
                    // Buffer Type 3 (VPS/SPS/PPS) - don't write yet
                    header_buffer.assign(frame->data, frame->data + frame->data_len);
                    printf("Header buffered, size=%zu bytes\n", header_buffer.size());
                }
                else if (frame->type == 2 && !header_buffer.empty()) {
                    // Combine header + IDR and write together
                    std::vector<uint8_t> combined;
                    combined.reserve(header_buffer.size() + frame->data_len);
                    combined.insert(combined.end(), header_buffer.begin(), header_buffer.end());
                    combined.insert(combined.end(), frame->data, frame->data + frame->data_len);

                    int ret = -1;

                    if (streamer_file.is_ready())
                        ret = streamer_file.write_video_frame(combined.data(), combined.size(), pts,
                            true);

                    if (streamer_rtsp.is_ready())
                        ret = streamer_rtsp.write_video_frame(combined.data(), combined.size(), pts,
                            true);

                    if (ret == 0) {
                        recording_started = true;
                        printf("First frame written (header+IDR), total size=%zu bytes, recording started\n",
                               combined.size());
                    } else {
                        printf("Failed to write first frame (header+IDR): error %d\n", ret);
                    }
                    header_buffer.clear();
                }
                // Discard Type 1 (P-frames) until we have header+IDR written
            }
            else {
                // After recording started, write all subsequent frames normally
                if (streamer_file.is_ready())
                    streamer_file.write_video_frame(frame->data, frame->data_len, pts, frame->type == 2);
                if (streamer_rtsp.is_ready())
                    streamer_rtsp.write_video_frame(frame->data, frame->data_len, pts, frame->type == 2);
            }

            // blink
            if (counter++ > 100) {
                counter = 0;

                if (output_file_detections) {
                    fflush(output_file_detections);
                    fsync(fileno(output_file_detections));
                }

                std::thread([ipcmsg_handle]() {
                    uint8_t state = 1;
                    auto pReq = kd_ipcmsg_create_message(0, MSG_CMD_LED_SET, &state, sizeof(state));
                    auto ret = kd_ipcmsg_send_only(ipcmsg_handle, pReq);
                    kd_ipcmsg_destroy_message(pReq);

                    usleep(200000);

                    state = 0;
                    pReq = kd_ipcmsg_create_message(0, MSG_CMD_LED_SET, &state, sizeof(state));
                    ret = kd_ipcmsg_send_only(ipcmsg_handle, pReq);
                    kd_ipcmsg_destroy_message(pReq);
                }).detach();
            }

            s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_READ_DONE, pBuf);
            if (K_SUCCESS != s32Ret) {
                printf("read done error:%x\n", s32Ret);
                break;
            }

            std::vector<DetectionNormalizedCommon>  _pending_detections;
            uint64_t _pending_detections_pts;
            {
                std::lock_guard lock(pending_detections_mutex);

                if (pending_detections_pts == UINT64_MAX    // No pending detections
                    || first_frame_time_stamp == UINT64_MAX) // Did not initialize fist pts
                    _pending_detections_pts = UINT64_MAX;
                else {
                    // If we have the first pts and pending_detections_pts is valid, then we got a detection result.
                    // In this case take these values to process it
                    _pending_detections_pts = pending_detections_pts - first_frame_time_stamp;
                    _pending_detections = std::move(pending_detections);
                    pending_detections_pts = UINT64_MAX;
                }
            }

            if (_pending_detections_pts != UINT64_MAX) {
                auto len = snprintf(common_buf, sizeof(common_buf), "%lu;", _pending_detections_pts/1000);
                for (auto &it: _pending_detections) {
                    auto l = snprintf(common_buf+len, sizeof(common_buf)-len, "%s %.2f %.10f %.10f %f %f;",
                        detect_classes[it.class_id].c_str(), it.confidence, it.x, it.y, it.w, it.h);
                    if (l >= 0)
                        len += l;
                }
                len += snprintf(common_buf+len, sizeof(common_buf)-len, "\n");

                if (output_file_detections)
                    fwrite(common_buf, 1, len, output_file_detections);

                std::lock_guard<std::mutex> lock(stream_endpoint_mutex);
                if (stream_endpoint_detections.port() != 0) {
                    udp_socket->send_to(asio::buffer(common_buf, len), stream_endpoint_detections);
                }
            }
        }
        else {
            usleep(10000);
        }
    }

    streamer_file.stop();
    if (output_file_detections) fclose(output_file_detections);
    // Do dot stop rtsp beause of to long
    //streamer_rtsp.stop();

    printf("read_fifo finished\n");
}

void udp_receiver(asio::ip::udp::socket *socket) {
    char recv_buf[64];
    asio::ip::udp::endpoint sender_endpoint;

    while (socket->is_open()) {
        asio::error_code error;
        // TODO: receive_from does not exit on socket close
        size_t len = socket->receive_from(asio::buffer(recv_buf), sender_endpoint, 0, error);

        if (!error && len > 0) {
            switch (recv_buf[0]) {
                case 'd': {
                    std::lock_guard<std::mutex> lock(stream_endpoint_mutex);
                    if (stream_endpoint_detections != sender_endpoint) {
                        stream_endpoint_detections = sender_endpoint;
                        std::cout << "Stream received detections: " << stream_endpoint_detections.address().to_string()
                                << ":" << stream_endpoint_detections.port() << std::endl;
                    }
                }
                break;
                default: ;
            }
        }
    }

    printf("udp_receiver finished\n");
}

void tcp_server_accept(asio::ip::tcp::acceptor* acceptor, k_s32 ipcmsg_handle) {
    acceptor->async_accept([acceptor, ipcmsg_handle](asio::error_code ec, asio::ip::tcp::socket peer) {
        if (!ec) {
            // Handle client in separate thread
            std::thread([peer = std::move(peer), ipcmsg_handle]() mutable {
                printf("Client connected\n");

                static k_char *buf = nullptr;
                if (buf == nullptr) {
                    buf = static_cast<k_char *>(malloc(DATAFIFO_FRONT_BLOCK_LEN));
                }
                size_t buff_len = 0;

                asio::error_code ec;
                while (!ec) {
                    size_t len = peer.read_some(asio::buffer(buf + buff_len, DATAFIFO_FRONT_BLOCK_LEN - buff_len), ec);

                    if (!ec && len > 0) {
                        buff_len += len;
                        printf("Received %zu bytes\n", buff_len);

                        if (buff_len >= DATAFIFO_FRONT_BLOCK_LEN) {
                            std::cerr << "!!!!!Buffer overload!!!!!!" << std::endl;
                            break;
                        }

                        auto msg = reinterpret_cast<MSG_CMD_DETECT_RGB_struct *>(buf);
                        const size_t image_data_len = (static_cast<size_t>(msg->width) * static_cast<size_t>(msg->height) * 3) / 2;
                        printf("Image size: %dx%d\n", msg->width, msg->height);

                        if (image_data_len + sizeof(MSG_CMD_DETECT_RGB_struct) == buff_len) {
                            printf("Image data received, image_data_len = %lu\n", image_data_len);
                            buff_len = 0;

                            // call write NULL to flush
                            k_s32 ret = kd_datafifo_write(hDataFifo[WRITER_INDEX], NULL);
                            if (K_SUCCESS != ret) {
                                printf("kd_datafifo_write error:%x\n", ret);
                                break;
                            }

                            k_u32 datafifo_avail_write_len = 0;
                            ret = kd_datafifo_cmd(hDataFifo[WRITER_INDEX], DATAFIFO_CMD_GET_AVAIL_WRITE_LEN,
                                                     &datafifo_avail_write_len);
                            if (K_SUCCESS != ret) {
                                printf("get available write len error:%x\n", ret);
                                break;
                            }
                            if (datafifo_avail_write_len >= DATAFIFO_FRONT_BLOCK_LEN) {
                                printf("About to send...\n");

                                ret = kd_datafifo_write(hDataFifo[WRITER_INDEX], buf + sizeof(MSG_CMD_DETECT_RGB_struct));
                                if (K_SUCCESS != ret) {
                                    printf("kd_datafifo_write error:%x\n", ret);
                                    break;
                                }

                                ret = kd_datafifo_cmd(hDataFifo[WRITER_INDEX], DATAFIFO_CMD_WRITE_DONE, NULL);
                                if (K_SUCCESS != ret) {
                                    printf("DATAFIFO_CMD_WRITE_DONE error:%x\n", ret);
                                    break;
                                }

                                auto pReq = kd_ipcmsg_create_message(0, MSG_CMD_DETECT_RGB, msg,
                                                                     sizeof(MSG_CMD_DETECT_RGB_struct));
                                k_ipcmsg_message_t *responce = nullptr;
                                ret = kd_ipcmsg_send_sync(ipcmsg_handle, pReq, &responce, 2000);
                                if (ret != K_SUCCESS) {
                                    printf("kd_ipcmsg_send_sync failed: %d\n", ret);
                                    break;
                                }
                                if (responce->u32CMD == MSG_CMD_DETECT_RGB && responce->s32RetVal == K_SUCCESS) {
                                    const size_t est_count = (responce->u32BodyLen - 1) / sizeof(DetectionCommon);
                                    uint16_t count = static_cast<uint8_t*>(responce->pBody)[0];
                                    if (count != est_count) {
                                        printf("Wrong esimated count! %hu %lu\n", count, est_count);
                                    }
                                    else {
                                        printf("MSG_CMD_DETECT_RGB success, %d\n", count);

                                        peer.write_some(asio::buffer(&count, sizeof(uint16_t)));
                                        peer.write_some(asio::buffer( static_cast<uint8_t*>(responce->pBody) + 1, responce->u32BodyLen-1));
                                        peer.wait(asio::ip::tcp::socket::wait_write);
                                    }

                                    asio::error_code shutdown_ec;
                                    peer.shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_ec);
                                    peer.close();
                                }
                                kd_ipcmsg_destroy_message(responce);
                                kd_ipcmsg_destroy_message(pReq);
                            } else {
                                printf("no free space: %d\n", datafifo_avail_write_len);
                            }
                        }
                    }
                }

                printf("Client disconnected\n");
            }).detach();
        }

        // Accept next connection (RECURSIVE CALL)
        tcp_server_accept(acceptor, ipcmsg_handle);
    });
}

static void ipcmsg_recv(k_s32 s32Id, k_ipcmsg_message_t* msg)
{
    printf("ipcmsg_recv %lu\n", msg->u32CMD);
    switch (msg->u32CMD) {
        case MSG_CMD_DETECTIONS: {
            auto pts = static_cast<uint64_t*>(msg->pBody);
            size_t count = (msg->u32BodyLen - sizeof(uint64_t)) / sizeof(DetectionNormalizedCommon);
            auto detections_p = reinterpret_cast<DetectionNormalizedCommon*>(static_cast<uint8_t*>(msg->pBody) + sizeof(uint64_t));

            std::lock_guard lock(pending_detections_mutex);
            pending_detections.clear();
            pending_detections_pts = *pts;
            for (size_t i = 0; i < count; i++) {
                auto det = &detections_p[i];
                pending_detections.push_back(*det);
            }
        } break;
        default:
            break;
    }
}

int main(int argc, char *argv[]) {
    std::cout << "./driver_assistant_front -H to show usage" << std::endl;
    std::cout << "./driver_assistant_front -b /mnt/bb" << std::endl;

    std::optional<std::string> bb_dir_path;
    bool daemon_mode;
    int ret = parse_config(argc, argv, bb_dir_path, daemon_mode);

    k_u64 datafifo_phy_addr[2] = {0,0};

    k_ipcmsg_connect_t stConnectAtt {
        .u32RemoteId = 1,
        .u32Port = 101,
        .u32Priority = 0
    };

    k_s32 ipcmsg_handle;
    ret = kd_ipcmsg_add_service(IPCMSG_NAME, &stConnectAtt);
    if (ret != K_SUCCESS) {
        printf("kd_ipcmsg_add_service failed: %d\n", ret);
        return -1;
    }
    printf("kd_ipcmsg_connect...\n");
    ret = kd_ipcmsg_connect(&ipcmsg_handle, IPCMSG_NAME, ipcmsg_recv);
    if (ret != K_SUCCESS) {
        printf("kd_ipcmsg_connect failed: %d\n", ret);
        return -1;
    }
    std::thread ipcmsg_thread([ipcmsg_handle] {
        kd_ipcmsg_run(ipcmsg_handle);
    });
    // request datafifo_phy_addr
    auto pReq = kd_ipcmsg_create_message(0, MSG_CMD_GET_PHY_ADDRESS, nullptr, 0);
    k_ipcmsg_message_t *responce = nullptr;
    ret = kd_ipcmsg_send_sync(ipcmsg_handle, pReq, &responce, 60*1000);
    if (ret != K_SUCCESS) {
        printf("kd_ipcmsg_send_sync failed: %d\n", ret);
    }
    else if (responce->u32CMD == MSG_CMD_GET_PHY_ADDRESS && responce->s32RetVal == K_SUCCESS && responce->u32BodyLen == sizeof(datafifo_phy_addr)) {
        auto p = reinterpret_cast<k_u64*>(responce->pBody);
        datafifo_phy_addr[WRITER_INDEX] = p[0];
        datafifo_phy_addr[READER_INDEX] = p[1];
    }
    kd_ipcmsg_destroy_message(responce);
    kd_ipcmsg_destroy_message(pReq);

    printf("datafifo WRITER_INDEX = %lx, READER_INDEX = %lx\n", datafifo_phy_addr[WRITER_INDEX], datafifo_phy_addr[READER_INDEX]);
    if (datafifo_phy_addr[WRITER_INDEX] == 0 || datafifo_phy_addr[READER_INDEX] == 0) {
        printf("datafifo_phy_addr not received!\n");
        kd_ipcmsg_disconnect(ipcmsg_handle);
        return -1;
    }

    std::optional<BbFiles_t> bb_files_path;
    if (bb_dir_path.has_value()) {
        for (int i = 0; i < 0xFFFF; ++i) {
            char bb_file_buf[64];
            char bb_log_buf[64];
            snprintf(bb_file_buf, sizeof(bb_file_buf), "%s/%d.mp4", bb_dir_path->c_str(), i);
            snprintf(bb_log_buf, sizeof(bb_log_buf), "%s/%d.txt", bb_dir_path->c_str(), i);

            FILE *file = fopen(bb_file_buf, "r");
            if (!file) {
                file = fopen(bb_log_buf, "r");
                if (!file) {
                    bb_files_path = BbFiles_t {
                        .video = bb_file_buf,
                        .log = bb_log_buf
                    };
                    break;
                }
                fclose(file);
            }
            else fclose(file);
        }
    }

    ret = datafifo_init(datafifo_phy_addr[READER_INDEX], datafifo_phy_addr[WRITER_INDEX]);

    asio::io_context io_context;
    // UDP Server
    asio::ip::udp::socket socket(io_context, asio::ip::udp::endpoint(asio::ip::udp::v4(), 5555));
    std::thread udp_receiver_thread(udp_receiver, &socket);
    // TCP Server
    asio::ip::tcp::acceptor acceptor(io_context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 5555));
    tcp_server_accept(&acceptor, ipcmsg_handle);
    std::thread io_context_thread([&io_context]() {
        io_context.run();
    });

    std::thread read_fifo_thread(read_fifo, &socket, ipcmsg_handle, bb_files_path);

    if (!daemon_mode) {
        printf("Input q to exit: \n");
        while (getchar() != 'q') {
            usleep(10000);
        }

        send_stop = true;
    }

    read_fifo_thread.join();

    socket.close();
    acceptor.close();
    // TODO: thread dost not stop!!!
    udp_receiver_thread.join();

    datafifo_deinit();

    kd_ipcmsg_disconnect(ipcmsg_handle);
    kd_ipcmsg_del_service(IPCMSG_NAME);
    ipcmsg_thread.join();

    return 0;
}