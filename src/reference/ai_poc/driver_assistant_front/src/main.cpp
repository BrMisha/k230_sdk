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
#include <argparse/argparse.hpp>

#include "utils.h"
#include "black_box.h"
#include "fb_display.h"
#include "k_datafifo.h"
#include "k_ipcmsg.h"
#include "../../driver_assistant_detector/common_ipc.h"
#include "media_streamer_file.h"  // From media_streaming module
#include "media_streamer_rtsp.h"  // From media_streaming module
#include "websocket_server.h"

// LVGL includes
extern "C" {
#include "lvgl.h"
#include "src/osal/lv_os.h"  // For lv_lock()/lv_unlock()
#include "lv_port_disp.h"
#include "ui/export/ui.h"
}

using namespace std::chrono_literals;
using namespace driver_assistant_detector;

// datafifo
#define READER_INDEX    0
#define WRITER_INDEX    1
static k_datafifo_handle hDataFifo[2] = {
    (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE, (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE
};

std::atomic<bool> send_stop(false);

std::vector<DetectionNormalizedCommon>  pending_detections;
std::vector<DetectionNormalizedCommon>  pending_pre_detections;
uint64_t pending_detections_pts = UINT64_MAX;
DetectedSituation pending_detections_situation;
std::mutex pending_detections_mutex;

std::mutex stream_endpoint_mutex;
asio::ip::udp::endpoint stream_endpoint_detections;

//FbDisplay g_fb_display;

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

void read_fifo(asio::ip::udp::socket *udp_socket, k_s32 ipcmsg_handle, const std::optional<std::string> &bb_dir_path, websocket_server::WebSocketServer *ws_server = nullptr) {
    k_u32 readLen = 0;
    k_s32 s32Ret = K_SUCCESS;
    int counter = 0;

    std::unique_ptr<black_box> streamer_file;
    if (bb_dir_path.has_value()) {
        streamer_file = std::make_unique<black_box>(bb_dir_path.value(), 1920, 1080);
    }

    MediaStreamerRtsp streamer_rtsp;
    streamer_rtsp.init("live", 1920, 1080);

    bool recording_started = false;
    std::vector<uint8_t> header_buffer;
    uint64_t header_buffer_pts = 0;  // PTS of buffered HEADER (for stale data detection)
    std::vector<uint8_t> periodic_header_buffer;  // Buffer for periodic HEADER frames

    while (!send_stop) {
        readLen = 0;
        s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_GET_AVAIL_READ_LEN, &readLen);
        if (K_SUCCESS != s32Ret) {
            printf("get available read len error:%x\n", s32Ret);
            break;
        }

        if (readLen > 0) {
            auto start_time = std::chrono::steady_clock::now();

            k_char *pBuf;
            s32Ret = kd_datafifo_read(hDataFifo[READER_INDEX], reinterpret_cast<void **>(&pBuf));
            if (K_SUCCESS != s32Ret) {
                printf("read error:%x\n", s32Ret);
                break;
            }

            auto frame = reinterpret_cast<DataFifoFrame_t*>(pBuf);

            /*uint64_t microseconds = pts;
            uint64_t milliseconds = microseconds / 1000;
            uint64_t seconds = milliseconds / 1000;
            uint64_t minutes = seconds / 60;
            uint64_t hours = minutes / 60;
            printf("Read frame. pts: %lu %02lu:%02lu:%02lu.%03lu, type %d, len %u\n", pts,
                   hours, minutes % 60, seconds % 60, milliseconds % 1000, frame->type, frame->data_len);*/

            if (!recording_started) {
                if (frame->type == 3) {
                    // Buffer Type 3 (VPS/SPS/PPS) - don't write yet
                    header_buffer.assign(frame->data, frame->data + frame->data_len);
                    header_buffer_pts = frame->pts;  // Store HEADER PTS for validation
                    printf("Header buffered, size=%zu bytes, PTS=%lu\n", header_buffer.size(), frame->pts);
                }
                else if (frame->type == 2 && !header_buffer.empty()) {
                    // Validate HEADER and I-frame PTS are close (within 2 seconds)
                    // This detects stale HEADER frames from previous encoder sessions
                    uint64_t pts_diff = (frame->pts > header_buffer_pts)
                                      ? (frame->pts - header_buffer_pts)
                                      : (header_buffer_pts - frame->pts);

                    if (pts_diff > 2000000) {  // More than 2 seconds apart
                        printf("WARNING: Stale HEADER detected (PTS gap = %lu us = %.1f sec)\n",
                               pts_diff, pts_diff / 1000000.0);
                        printf("  HEADER PTS: %lu, I-frame PTS: %lu\n", header_buffer_pts, frame->pts);
                        printf("  Discarding stale HEADER, waiting for fresh one\n");
                        header_buffer.clear();
                        header_buffer_pts = 0;
                        // Don't start recording yet - wait for next HEADER that matches I-frame PTS
                    }
                    else {
                    // PTS gap is acceptable - HEADER and I-frame are from same session
                    // Combine header + IDR and write together
                    std::vector<uint8_t> combined;
                    combined.reserve(header_buffer.size() + frame->data_len);
                    combined.insert(combined.end(), header_buffer.begin(), header_buffer.end());
                    combined.insert(combined.end(), frame->data, frame->data + frame->data_len);

                    int ret = -1;

                    if (streamer_file != nullptr)
                        ret = streamer_file->write_video_frame(combined.data(), combined.size(), frame->pts,
                            true);

                    if (streamer_rtsp.is_ready())
                        ret = streamer_rtsp.write_video_frame(combined.data(), combined.size(), frame->pts,
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
                }
                // Discard Type 1 (P-frames) until we have header+IDR written
            }
            else {
                // After recording started: combine periodic HEADERs with I-frames, write P-frames normally
                if (frame->type == 3) {
                    // Periodic HEADER frame - buffer it to combine with next I-frame
                    // I-frames need their VPS/SPS/PPS to decode properly
                    periodic_header_buffer.assign(frame->data, frame->data + frame->data_len);
                }
                else if (frame->type == 2 && !periodic_header_buffer.empty()) {
                    // I-frame following HEADER - combine them
                    std::vector<uint8_t> combined;
                    combined.reserve(periodic_header_buffer.size() + frame->data_len);
                    combined.insert(combined.end(), periodic_header_buffer.begin(), periodic_header_buffer.end());
                    combined.insert(combined.end(), frame->data, frame->data + frame->data_len);

                    if (streamer_file != nullptr)
                        streamer_file->write_video_frame(combined.data(), combined.size(), frame->pts, true);
                    if (streamer_rtsp.is_ready())
                        streamer_rtsp.write_video_frame(combined.data(), combined.size(), frame->pts, true);

                    periodic_header_buffer.clear();
                }
                else {
                    // P-frame or I-frame without header - write normally
                    if (streamer_file != nullptr)
                        streamer_file->write_video_frame(frame->data, frame->data_len, frame->pts, frame->type == 2);
                    if (streamer_rtsp.is_ready())
                        streamer_rtsp.write_video_frame(frame->data, frame->data_len, frame->pts, frame->type == 2);

                    // Log standalone I-frames (shouldn't happen)
                    if (frame->type == 2) {
                        printf("WARNING: Standalone I-frame without HEADER (type 2), size=%u bytes, PTS=%lu\n",
                               frame->data_len, frame->pts);
                    }
                }
            }

            // blink
            if (counter++ > 100) {
                counter = 0;

                if (streamer_file) {
                    streamer_file->flush_files();
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

            std::vector<DetectionNormalizedCommon>  _pending_pre_detections;
            std::vector<DetectionNormalizedCommon>  _pending_detections;
            DetectedSituation _pending_detections_situation;
            uint64_t _pending_detections_pts;
            {
                std::lock_guard lock(pending_detections_mutex);

                if (pending_detections_pts == UINT64_MAX)    // No pending detections
                    _pending_detections_pts = UINT64_MAX;
                else {
                    // If we have the first pts and pending_detections_pts is valid, then we got a detection result.
                    // In this case take these values to process it
                    _pending_detections_pts = pending_detections_pts;
                    _pending_pre_detections = std::move(pending_pre_detections);
                    _pending_detections = std::move(pending_detections);
                    _pending_detections_situation = pending_detections_situation;
                    pending_detections_pts = UINT64_MAX;
                }
            }

            if (_pending_detections_pts != UINT64_MAX) {
                if (streamer_file) {
                    streamer_file->write_detections(_pending_detections_pts, _pending_pre_detections, _pending_detections, _pending_detections_situation);
                }
                // Broadcast via WebSocket
                if (ws_server) {
                    ws_server->broadcast_detections(_pending_detections_pts, _pending_detections_situation, _pending_detections);
                }

                // TODO: impl in future
                /*std::lock_guard<std::mutex> lock(stream_endpoint_mutex);
                if (stream_endpoint_detections.port() != 0) {
                    udp_socket->send_to(asio::buffer(common_buf, len), stream_endpoint_detections);
                }*/
            }

            auto duration_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start_time).count();
            if (duration_ms > 10.0) {
                printf("WARNING: Frame processing took %.2f ms\n", duration_ms);
            }
        }
        else {
            usleep(10000);
        }
    }

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

void tcp_server_accept(asio::ip::tcp::acceptor* acceptor, k_s32 ipcmsg_handle, websocket_server::WebSocketServer *ws_server = nullptr) {
    acceptor->async_accept([acceptor, ipcmsg_handle, ws_server](asio::error_code ec, asio::ip::tcp::socket peer) {
        if (!ec) {
            // Handle client in separate thread
            std::thread([peer = std::move(peer), ipcmsg_handle, ws_server]() mutable {
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
                        const size_t image_data_len = static_cast<size_t>(msg->width) * static_cast<size_t>(msg->height) * 3;
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
                                ret = kd_ipcmsg_send_sync(ipcmsg_handle, pReq, &responce, 10000);
                                if (ret != K_SUCCESS) {
                                    printf("kd_ipcmsg_send_sync failed: %d\n", ret);
                                    break;
                                }
                                if (responce->u32CMD == MSG_CMD_DETECT_RGB && responce->s32RetVal == K_SUCCESS) {
                                    auto responce_struct = reinterpret_cast<MSG_CMD_DETECT_RGB_responce_struct*>(responce->pBody);
                                    const size_t est_count = sizeof(MSG_CMD_DETECT_RGB_responce_struct) + (sizeof(DetectionNormalizedCommon) * responce_struct->detections_count);
                                    if (responce->u32BodyLen != est_count) {
                                        printf("Wrong esimated count! %hu %lu\n", responce_struct->detections_count, est_count);
                                    }
                                    else {
                                        printf("MSG_CMD_DETECT_RGB success, %d %d\n", responce_struct->detections_count, responce_struct->situation.color);

                                        peer.write_some(asio::buffer(static_cast<uint8_t*>(responce->pBody), responce->u32BodyLen));
                                        peer.wait(asio::ip::tcp::socket::wait_write);

                                        if (ws_server) {
                                            std::vector<DetectionNormalizedCommon> detections;
                                            for (int i = 0; i < responce_struct->detections_count; i++)
                                                detections.push_back(responce_struct->detections[i]);

                                            ws_server->broadcast_detections(0, responce_struct->situation, detections);
                                            //g_fb_display.drawSituation(responce_struct->situation);
                                        }
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
        tcp_server_accept(acceptor, ipcmsg_handle, ws_server);
    });
}

static void ipcmsg_recv(k_s32 s32Id, k_ipcmsg_message_t* msg)
{
    //printf("ipcmsg_recv %lu\n", msg->u32CMD);
    switch (msg->u32CMD) {
        case MSG_CMD_DETECTIONS: {
            auto data = reinterpret_cast<MSG_CMD_DETECTIONS_struct*>(msg->pBody);

            {
                std::lock_guard lock(pending_detections_mutex);
                pending_detections.clear();
                pending_detections_pts = data->pts;
                pending_detections_situation = data->situation;
                //g_fb_display.drawSituation(data->situation);
                for (size_t i = 0; i < data->detections_count; i++) {
                    pending_detections.push_back(data->detections[i]);
                }
                for (size_t i = 0; i < data->detections_pre_process_count; i++) {
                    // pre_detections locates after the last data->detections
                    pending_pre_detections.push_back(data->detections[static_cast<size_t>(data->detections_count) + i]);
                }
            }

            static DetectedSituation   last_situation;
            static int color_timeout = 0;

            if (last_situation != data->situation || color_timeout != 0) {
                last_situation = data->situation;

                if (color_timeout != 0) {
                    --color_timeout;
                }

                lv_lock();

                if (color_timeout == 0 || last_situation.color != DetectedSituationColor::NONE) {
                    switch (last_situation.color) {
                        case DetectedSituationColor::RED:
                            lv_obj_set_style_bg_color(ui_color, lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
                            color_timeout = 2;
                            break;
                        case DetectedSituationColor::GREEN:
                            lv_obj_set_style_bg_color(ui_color, lv_palette_main(LV_PALETTE_GREEN), LV_PART_MAIN);
                            color_timeout = 2;
                            break;
                        case DetectedSituationColor::YELLOW:
                            lv_obj_set_style_bg_color(ui_color, lv_palette_main(LV_PALETTE_YELLOW), LV_PART_MAIN);
                            color_timeout = 2;
                            break;
                        case DetectedSituationColor::NONE:
                        default:
                            lv_obj_set_style_bg_color(ui_color, lv_color_hex(0x404040), LV_PART_MAIN);
                            color_timeout = 0;
                            break;
                    }
                }

                // Update arrow visibility based on detected situation
                if (last_situation.arrow_left) {
                    lv_obj_remove_flag(ui_arrowleft, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(ui_arrowleft, LV_OBJ_FLAG_HIDDEN);
                }

                if (last_situation.arrow_right) {
                    lv_obj_remove_flag(ui_arrowright, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(ui_arrowright, LV_OBJ_FLAG_HIDDEN);
                }

                if (last_situation.arrow_forward) {
                    lv_obj_remove_flag(ui_arrowforward, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(ui_arrowforward, LV_OBJ_FLAG_HIDDEN);
                }

                lv_unlock();
            }
        } break;
        default:
            break;
    }
}

int main(int argc, char *argv[]) {
    std::cout << "Built at " << __DATE__ << " " << __TIME__ << std::endl;

    // Parse command line arguments using argparse
    std::optional<std::string> bb_dir_path;
    bool daemon_mode = false;

    argparse::ArgumentParser program("driver_assistant_front");

    program.add_argument("-b", "--bb")
        .help("Black box output directory path")
        .action([&](const std::string& value) { bb_dir_path = value; });

    program.add_argument("-d", "--daemon")
        .flag()
        .help("Run in daemon mode")
        .store_into(daemon_mode);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    // Initialize LVGL
    lv_init();
    // Initialize display port
    lv_port_disp_init();
    // Load SquareLine Studio UI
    ui_init();

    // Create LVGL timer thread (30ms tick + 1s time update)
    std::thread lvgl_thread([]() {
        while (!send_stop) {
            lv_tick_inc(30);        // Increment LVGL tick by 30ms
            lv_timer_handler();
            usleep(30000);          // Sleep 30ms
        }
    });

    std::thread lvgl_thread_upd([]() {
        static char ip_buffer[32];
        static char time_buffer[16];
        static char cpu_buffer[8];

        while (!send_stop) {
            lv_lock();

            // Set IP address on display
            std::string ip = utils::get_ip_address();
            snprintf(ip_buffer, sizeof(ip_buffer), "%s", ip.c_str());
            lv_label_set_text(ui_ipaddr, ip_buffer);

            // Set time on display
            std::string time = utils::get_current_time();
            snprintf(time_buffer, sizeof(time_buffer), "%s", time.c_str());
            lv_label_set_text(ui_time, time_buffer);

            // Set Linux CPU load on display
            int cpu_load = utils::get_linux_cpu_load();
            snprintf(cpu_buffer, sizeof(cpu_buffer), "%d", cpu_load);
            lv_label_set_text(ui_cpu1, cpu_buffer);

            //lv_refr_now(NULL);
            lv_unlock();

            usleep(1000000);
        }
    });

    int ret;

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

    ret = datafifo_init(datafifo_phy_addr[READER_INDEX], datafifo_phy_addr[WRITER_INDEX]);

    // WebSocket Server
    websocket_server::WebSocketServer ws_server(8080, "/www");
    std::thread websocket_thread([&ws_server]() {
        ws_server.run();
    });

    asio::io_context io_context;
    // UDP Server
    asio::ip::udp::socket socket(io_context, asio::ip::udp::endpoint(asio::ip::udp::v4(), 5555));
    std::thread udp_receiver_thread(udp_receiver, &socket);
    // TCP Server
    asio::ip::tcp::acceptor acceptor(io_context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 5555));
    tcp_server_accept(&acceptor, ipcmsg_handle, &ws_server);
    std::thread io_context_thread([&io_context]() {
        io_context.run();
    });

    std::thread read_fifo_thread(read_fifo, &socket, ipcmsg_handle, bb_dir_path, &ws_server);

    if (!daemon_mode) {
        printf("Input q to exit: \n");
        while (getchar() != 'q') {
            usleep(10000);
        }

        send_stop = true;

        auto pReq = kd_ipcmsg_create_message(0, MSG_CMD_APP_CLOSED, "", 1);
        auto ret = kd_ipcmsg_send_only(ipcmsg_handle, pReq);
        kd_ipcmsg_destroy_message(pReq);
    }

    read_fifo_thread.join();

    ws_server.stop();
    websocket_thread.join();

    socket.close();
    acceptor.close();
    // TODO: thread dost not stop!!!
    udp_receiver_thread.join();

    datafifo_deinit();

    kd_ipcmsg_disconnect(ipcmsg_handle);
    kd_ipcmsg_del_service(IPCMSG_NAME);
    ipcmsg_thread.join();

    // Cleanup LVGL
    lvgl_thread_upd.join();
    lvgl_thread.join();
    ui_destroy();
    lv_port_disp_deinit();

    return 0;
}