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

// datafifo
#define READER_INDEX    0
#define WRITER_INDEX    1
static k_datafifo_handle hDataFifo[2] = {
    (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE, (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE
};

std::atomic<bool> send_stop(false);

using namespace std::chrono_literals;

FILE *output_file_video = nullptr;
FILE *output_file_detections = nullptr;

std::mutex stream_endpoint_mutex;
asio::ip::udp::endpoint stream_endpoint_video;
asio::ip::udp::endpoint stream_endpoint_detections;

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

void read_fifo(asio::ip::udp::socket *udp_socket, k_s32 ipcmsg_handle) {
    k_u32 readLen = 0;
    k_s32 s32Ret = K_SUCCESS;
    int counter = 0;

    MediaStreamerFile streamer_file;
    streamer_file.init("/mnt/bb/recording.mp4", 1920, 1080);

    bool recording_started = false;
    std::vector<uint8_t> header_buffer;

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

            uint64_t microseconds = frame->pts;
            uint64_t milliseconds = microseconds / 1000;
            uint64_t seconds = milliseconds / 1000;
            uint64_t minutes = seconds / 60;
            uint64_t hours = minutes / 60;
            //printf("Read frame. pts: %02lu:%02lu:%02lu.%03lu, type %d, len %u\n",
            //       hours, minutes % 60, seconds % 60, milliseconds % 1000, frame->type, frame->data_len);

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

                    int ret = streamer_file.write_video_frame(combined.data(), combined.size(),
                                                         frame->pts, true);
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
                streamer_file.write_video_frame(frame->data, frame->data_len, frame->pts, frame->type == 2);
            }

            /*
            auto pBuf_ = pBuf;

            auto detections_count = ((uint16_t *) pBuf)[0];
            k_char *detections_buffer = pBuf;
            pBuf += 2;
            std::vector<DetectionCommon> detections;
            if (detections_count != UINT16_MAX) {
                for (uint16_t i = 0; i < detections_count; i++) {
                    DetectionCommon d;
                    memcpy(&d, pBuf, sizeof(DetectionCommon));
                    pBuf += sizeof(DetectionCommon);
                    detections.push_back(d);
                }
            }

            unsigned long pts = ((unsigned long *) pBuf)[0];
            unsigned int len = ((unsigned int *) pBuf)[2];
            k_char *data = pBuf + sizeof(unsigned long) + sizeof(unsigned int);

            if (output_file_video && output_file_detections) {
                fwrite(data, 1, len, output_file_video);

                if (detections_count != UINT16_MAX) {
                    fprintf(output_file_detections, "%lu;", pts);
                    for (auto &it: detections) {
                        fprintf(output_file_detections, "%s %.2f %d %d %d %d;", detect_classes[it.class_id].c_str(),
                                it.confidence, it.x, it.y, it.w, it.h);
                    }
                    fprintf(output_file_detections, "\n");
                }
            }

            if (counter++ > 10) {
                counter = 0;

                if (output_file_video && output_file_detections) {
                    fflush(output_file_video);
                    fflush(output_file_detections);
                    fsync(fileno(output_file_video));
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

            {
                std::lock_guard<std::mutex> lock(stream_endpoint_mutex);
                if (stream_endpoint_detections.port() != 0 && detections_count != UINT16_MAX) {
                    udp_socket->send_to(
                        asio::buffer(detections_buffer, 2 + (detections.size() * sizeof(DetectionCommon))),
                        stream_endpoint_detections);
                }
                if (stream_endpoint_video.port() != 0) {
                    const unsigned int MAX = 50000;
                    for (unsigned int i = 0; i < len;) {
                        auto sent = std::min(MAX, len - i);
                        udp_socket->send_to(asio::buffer(data + static_cast<size_t>(i), sent), stream_endpoint_video);
                        i += sent;
                    }
                }
            }

            printf("Timestamp: %lu, len: %d\n", pts, len);
            if (detections.size() > 0) {
                printf("    Received %zu detections:\n", detections.size());
                for (size_t i = 0; i < detections.size(); i++) {
                    const auto &det = detections[i];
                    printf("      Detection %zu: %s (conf=%.1f) at (%d,%d) size %dx%d\n",
                           i + 1, detect_classes[det.class_id].c_str(), det.confidence,
                           det.x, det.y, det.w, det.h);
                }
            }*/

            s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_READ_DONE, pBuf);
            if (K_SUCCESS != s32Ret) {
                printf("read done error:%x\n", s32Ret);
                break;
            }
        }
        else {
            usleep(10000);
        }
    }

    streamer_file.stop();

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
                case 'v': {
                    std::lock_guard<std::mutex> lock(stream_endpoint_mutex);
                    if (stream_endpoint_video != sender_endpoint) {
                        stream_endpoint_video = sender_endpoint;
                        std::cout << "Stream received video: " << stream_endpoint_video.address().to_string()
                                << ":" << stream_endpoint_video.port() << std::endl;
                    }
                }
                break;
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

            // Convert PTS from microseconds to human-readable time format
            uint64_t microseconds = *pts;
            uint64_t milliseconds = microseconds / 1000;
            uint64_t seconds = milliseconds / 1000;
            uint64_t minutes = seconds / 60;
            uint64_t hours = minutes / 60;

            printf("Detections: %lu, pts: %02lu:%02lu:%02lu.%03lu\n",
                   count, hours, minutes % 60, seconds % 60, milliseconds % 1000);
            for (size_t i = 0; i < count; i++) {
                auto det = &detections_p[i];
                std::cout << "\t" << (i + 1) << ": "
                                            << detect_classes[det->class_id]
                                            << "confidence=" << det->confidence << " "
                                            << "box=[" << det->x << "," << det->y << ","
                                            << det->w << "x" << det->h << "]"
                                            << std::endl;
            }

        } break;
        default:
            break;
    }
}

int main(int argc, char *argv[]) {
    std::cout << "./driver_assistant_front -H to show usage" << std::endl;
    std::cout << "./driver_assistant_front -b /mnt/bb" << std::endl;

    std::optional<std::string> bb_path;
    bool daemon_mode;
    int ret = parse_config(argc, argv, bb_path, daemon_mode);

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

    if (bb_path.has_value()) {
        for (int i = 0; i < 0xFFFF; ++i) {
            char filename[50];
            sprintf(filename, (*bb_path + "/%d.h265").c_str(), i);
            FILE *file = fopen(filename, "r");
            if (!file) {
                sprintf(filename, (*bb_path + "/%d.txt").c_str(), i);
                file = fopen(filename, "r");
                if (!file) {
                    sprintf(filename, (*bb_path + "/%d.h265").c_str(), i);
                    printf("output_file_video %s\n", filename);
                    output_file_video = fopen(filename, "wb");

                    sprintf(filename, (*bb_path + "/%d.txt").c_str(), i);
                    printf("output_file_detections %s\n", filename);
                    output_file_detections = fopen(filename, "w");

                    break;
                }
                fclose(file);
            } else fclose(file);
        }

        if (!output_file_video || !output_file_detections) {
            std::cerr << "Can't open video file!" << std::endl;
            kd_ipcmsg_disconnect(ipcmsg_handle);
            return -1;
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

    std::thread read_fifo_thread(read_fifo, &socket, ipcmsg_handle);

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

    if (output_file_video) fclose(output_file_video);
    if (output_file_detections) fclose(output_file_detections);

    kd_ipcmsg_disconnect(ipcmsg_handle);
    kd_ipcmsg_del_service(IPCMSG_NAME);
    ipcmsg_thread.join();

    return 0;
}