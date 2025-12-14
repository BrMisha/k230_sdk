#include <iostream>
#include <iomanip>
#include <cmath>
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
#include "imu.h"
#include "black_box.h"
#include "fb_display.h"
#include "datafifo_helper.h"
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

std::atomic<bool> send_stop(false);
std::atomic<bool> is_moving(false);

DatafifoHelper::PendingDetections pending_detections;

std::mutex stream_endpoint_mutex;
asio::ip::udp::endpoint stream_endpoint_detections;

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

void tcp_server_accept(asio::ip::tcp::acceptor* acceptor, k_s32 ipcmsg_handle, std::shared_ptr<DatafifoHelper> fifo_helper, websocket_server::WebSocketServer *ws_server = nullptr) {
    acceptor->async_accept([acceptor, ipcmsg_handle, fifo_helper, ws_server](asio::error_code ec, asio::ip::tcp::socket peer) {
        if (!ec) {
            // Handle client in separate thread
            std::thread([peer = std::move(peer), ipcmsg_handle, fifo_helper, ws_server]() mutable {
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

                            auto fifo_writer = fifo_helper->writer();

                            if (fifo_writer != std::nullopt) {
                                printf("About to send...\n");

                                if (!fifo_writer->write(buf + sizeof(MSG_CMD_DETECT_RGB_struct))) {
                                    break;
                                }

                                auto pReq = kd_ipcmsg_create_message(0, MSG_CMD_DETECT_RGB, msg,
                                                                     sizeof(MSG_CMD_DETECT_RGB_struct));
                                k_ipcmsg_message_t *responce = nullptr;
                                auto ret = kd_ipcmsg_send_sync(ipcmsg_handle, pReq, &responce, 10000);
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

                            }
                        }
                    }
                }

                printf("Client disconnected\n");
            }).detach();
        }

        // Accept next connection (RECURSIVE CALL)
        tcp_server_accept(acceptor, ipcmsg_handle, fifo_helper, ws_server);
    });
}

static void ipcmsg_recv(k_s32 s32Id, k_ipcmsg_message_t* msg)
{
    //printf("ipcmsg_recv %lu\n", msg->u32CMD);
    switch (msg->u32CMD) {
        case MSG_CMD_DETECTIONS: {
            auto data = reinterpret_cast<MSG_CMD_DETECTIONS_struct*>(msg->pBody);

            {
                std::lock_guard lock(pending_detections.mutex);
                pending_detections.detections.clear();
                pending_detections.pts = data->pts;
                pending_detections.situation = data->situation;
                //g_fb_display.drawSituation(data->situation);
                for (size_t i = 0; i < data->detections_count; i++) {
                    pending_detections.detections.push_back(data->detections[i]);
                }
                for (size_t i = 0; i < data->detections_pre_process_count; i++) {
                    // pre_detections locates after the last data->detections
                    pending_detections.pre_detections.push_back(data->detections[static_cast<size_t>(data->detections_count) + i]);
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
    bool imu_calibrate = false;

    argparse::ArgumentParser program("driver_assistant_front");

    program.add_argument("-b", "--bb")
        .help("Black box output directory path")
        .action([&](const std::string& value) { bb_dir_path = value; });

    program.add_argument("-d", "--daemon")
        .flag()
        .help("Run in daemon mode")
        .store_into(daemon_mode);

    program.add_argument("--imuc")
        .flag()
        .help("Run IMU manual calibration at startup")
        .store_into(imu_calibrate);

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    Imu imu;
    if (imu.init())
    {
        // Run VESC-style calibration if --imuc flag is set
        if (imu_calibrate)
        {
            imu.calibrate();
        }
        else
        {
            // Try to load saved calibration
            imu.load_calibration(Imu::DEFAULT_CALIBRATION_FILE);
        }

        std::cout << "IMU initialized (accel-only). Starting test loop (Ctrl+C to exit)..." << std::endl;
    }
    else
    {
        std::cerr << "IMU initialization failed, continuing without IMU test" << std::endl;
    }

    std::thread imu_thread([&]()
    {
        while (!send_stop && imu.is_initialized() && imu.is_calibrated())
        {
            auto data = imu.read();
            if (data)
            {
                RpyAngles rpy = imu.update(*data);
                float mag = std::sqrt(data->accel_x * data->accel_x + data->accel_y * data->accel_y);
                is_moving = imu.is_moving(*data, 2.0f); // X+Y magnitude threshold
                /*std::cout << "Roll: " << std::fixed << std::setprecision(1) << rpy.roll
                    << "  Pitch: " << rpy.pitch
                    << "  Mag: " << std::setprecision(2) << mag
                    << "  " << (is_moving ? "MOVING" : "STILL")
                    << std::endl;*/
            }

            usleep(50000); // 50ms = 20Hz (sufficient for accel-only)
        }
    });

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

    std::atomic<k_s32> ipcmsg_handle(0);

    std::thread lvgl_thread_upd([&ipcmsg_handle]() {
        static char ip_buffer[32];
        static char time_buffer[16];
        static char cpu_buffer[10];

        while (!send_stop) {
            usleep(1000000);

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
            // Set RT-Thread CPU load on display (request from big core via IPC)
            std::optional<uint8_t> rt_cpu_load;
            k_s32 handle = ipcmsg_handle.load();
            if (handle != 0) {
                auto pReq = kd_ipcmsg_create_message(0, MSG_CMD_GET_CPU_USAGE, nullptr, 0);
                k_ipcmsg_message_t *resp = nullptr;
                auto ret = kd_ipcmsg_send_sync(handle, pReq, &resp, 1000);
                if (ret == K_SUCCESS && resp && resp->s32RetVal == K_SUCCESS && resp->u32BodyLen == sizeof(uint8_t)) {
                    rt_cpu_load = *reinterpret_cast<uint8_t*>(resp->pBody);
                }
                if (resp) kd_ipcmsg_destroy_message(resp);
                kd_ipcmsg_destroy_message(pReq);
            }
            if (rt_cpu_load.has_value())
                snprintf(cpu_buffer, sizeof(cpu_buffer), "%d/%d", *rt_cpu_load, cpu_load);
            else
                snprintf(cpu_buffer, sizeof(cpu_buffer), "--/%d", cpu_load);
            lv_label_set_text(ui_cpu, cpu_buffer);

            lv_label_set_text(ui_moving, is_moving ? "M" : "");

            //lv_refr_now(NULL);
            lv_unlock();
        }
    });

    int ret;

    k_u64 datafifo_phy_addr_writer = 0;
    k_u64 datafifo_phy_addr_reader = 0;

    k_ipcmsg_connect_t stConnectAtt {
        .u32RemoteId = 1,
        .u32Port = 101,
        .u32Priority = 0
    };

    ret = kd_ipcmsg_add_service(IPCMSG_NAME, &stConnectAtt);
    if (ret != K_SUCCESS) {
        printf("kd_ipcmsg_add_service failed: %d\n", ret);
        return -1;
    }
    printf("kd_ipcmsg_connect...\n");
    k_s32 ipcmsg_handle_tmp;
    ret = kd_ipcmsg_connect(&ipcmsg_handle_tmp, IPCMSG_NAME, ipcmsg_recv);
    if (ret != K_SUCCESS) {
        printf("kd_ipcmsg_connect failed: %d\n", ret);
        return -1;
    }
    ipcmsg_handle = ipcmsg_handle_tmp;
    std::thread ipcmsg_thread([&ipcmsg_handle] {
        kd_ipcmsg_run(ipcmsg_handle);
    });
    // request datafifo_phy_addr
    auto pReq = kd_ipcmsg_create_message(0, MSG_CMD_GET_PHY_ADDRESS, nullptr, 0);
    k_ipcmsg_message_t *responce = nullptr;
    ret = kd_ipcmsg_send_sync(ipcmsg_handle, pReq, &responce, 60*1000);
    if (ret != K_SUCCESS) {
        printf("kd_ipcmsg_send_sync failed: %d\n", ret);
    }
    else if (responce->u32CMD == MSG_CMD_GET_PHY_ADDRESS && responce->s32RetVal == K_SUCCESS && responce->u32BodyLen == sizeof(k_u64)*2) {
        auto p = reinterpret_cast<k_u64*>(responce->pBody);
        datafifo_phy_addr_writer = p[0];
        datafifo_phy_addr_reader = p[1];
    }
    kd_ipcmsg_destroy_message(responce);
    kd_ipcmsg_destroy_message(pReq);

    printf("datafifo WRITER_INDEX = %lx, READER_INDEX = %lx\n", datafifo_phy_addr_writer, datafifo_phy_addr_reader);
    if (datafifo_phy_addr_writer == 0 || datafifo_phy_addr_reader == 0) {
        printf("datafifo_phy_addr not received!\n");
        kd_ipcmsg_disconnect(ipcmsg_handle);
        return -1;
    }

    // WebSocket Server
    websocket_server::WebSocketServer ws_server(8080, "/www");
    std::thread websocket_thread([&ws_server]() {
        ws_server.run();
    });

    asio::io_context io_context;
    // UDP Server
    asio::ip::udp::socket socket(io_context, asio::ip::udp::endpoint(asio::ip::udp::v4(), 5555));
    std::thread udp_receiver_thread(udp_receiver, &socket);

    auto fifo_helper = std::make_shared<DatafifoHelper>(datafifo_phy_addr_reader, datafifo_phy_addr_writer, std::move(bb_dir_path), &pending_detections, &socket, &ws_server);

    // TCP Server
    asio::ip::tcp::acceptor acceptor(io_context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 5555));
    tcp_server_accept(&acceptor, ipcmsg_handle.load(), fifo_helper, &ws_server);
    std::thread io_context_thread([&io_context]() {
        io_context.run();
    });

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

    socket.close();
    acceptor.close();

    // to stop using fifo and something else
    fifo_helper->stop();

    ws_server.stop();
    websocket_thread.join();
    // TODO: thread dost not stop!!!
    udp_receiver_thread.join();

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