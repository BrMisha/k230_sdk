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

#include "k_datafifo.h"
#include "k_ipcmsg.h"
#include "../../../../common/cdk/user/component/ipcmsg/include/k_ipcmsg.h"
#include "../../driver_assistant_detector/common.h"

// datafifo
#define READER_INDEX    0
#define WRITER_INDEX    1
static k_datafifo_handle hDataFifo[2] = {
    (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE, (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE
};

std::atomic<bool> send_stop(false);

using namespace std::chrono_literals;

FILE *output_file_video = NULL;
FILE *output_file_detections = NULL;

std::mutex stream_endpoint_mutex;
asio::ip::udp::endpoint stream_endpoint_video;
asio::ip::udp::endpoint stream_endpoint_detections;

static void release(void *pStream) {
    printf("release %p\n", pStream);
}

int datafifo_init(k_u64 reader_phyAddr, k_u64 writer_phyAddr) {
    k_s32 s32Ret = K_SUCCESS;
    k_datafifo_params_s params_reader = {10, DATAFIFO_BLOCK_LEN, K_TRUE, DATAFIFO_READER};
    s32Ret = kd_datafifo_open_by_addr(&hDataFifo[READER_INDEX], &params_reader, reader_phyAddr);
    if (K_SUCCESS != s32Ret) {
        printf("open datafifo error:%x\n", s32Ret);
        return -1;
    }

    k_datafifo_params_s params_writer = {10, DATAFIFO_BLOCK_LEN, K_TRUE, DATAFIFO_WRITER};
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

int parse_config(int argc, char *argv[], std::string &bb, bool &daemon_mode) {
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
                bb = optarg;
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

void read_fifo(asio::ip::udp::socket *udp_socket) {
    k_u32 readLen = 0;
    k_char *pBuf;
    k_s32 s32Ret = K_SUCCESS;

    while (!send_stop) {
        readLen = 0;
        s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_GET_AVAIL_READ_LEN, &readLen);
        if (K_SUCCESS != s32Ret) {
            printf("get available read len error:%x\n", s32Ret);
            break;
        }

        if (readLen > 0) {
            s32Ret = kd_datafifo_read(hDataFifo[READER_INDEX], (void **) &pBuf);
            if (K_SUCCESS != s32Ret) {
                printf("read error:%x\n", s32Ret);
                break;
            }
            // TODO: Call it only when pBuf does not need anymore
            s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_READ_DONE, pBuf);
            if (K_SUCCESS != s32Ret) {
                printf("read done error:%x\n", s32Ret);
                break;
            }

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

                fflush(output_file_video);
                fflush(output_file_detections);
                fsync(fileno(output_file_video));
                fsync(fileno(output_file_detections));
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
            }
        }
    }

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

static void ipcmsg_recv(k_s32 s32Id, k_ipcmsg_message_t* msg)
{
    printf("ipcmsg_recv %lu\n", msg->u32CMD);
}

int main(int argc, char *argv[]) {
    std::cout << "./driver_assistant_front -H to show usage" << std::endl;
    std::cout << "./driver_assistant_front -b /mnt/bb" << std::endl;

    std::string bb_path;
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

    for (int i = 0; i < 0xFFFF; ++i) {
        char filename[50];
        sprintf(filename, (bb_path + "/%d.h265").c_str(), i);
        FILE *file = fopen(filename, "r");
        if (!file) {
            sprintf(filename, (bb_path + "/%d.txt").c_str(), i);
            file = fopen(filename, "r");
            if (!file) {
                sprintf(filename, (bb_path + "/%d.h265").c_str(), i);
                printf("output_file_video %s\n", filename);
                output_file_video = fopen(filename, "wb");

                sprintf(filename, (bb_path + "/%d.txt").c_str(), i);
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

    ret = datafifo_init(datafifo_phy_addr[READER_INDEX], datafifo_phy_addr[WRITER_INDEX]);

    // example of fifo writer
    k_char buf[DATAFIFO_BLOCK_LEN];
    for (int o = 0; o < 10; ++o)
    {
        // call write NULL to flush
        k_s32 s32Ret = kd_datafifo_write(hDataFifo[WRITER_INDEX], NULL);
        if (K_SUCCESS != s32Ret)
        {
            printf("write error:%x\n", s32Ret);
        }

        k_u32 availWriteLen = 0;
        s32Ret = kd_datafifo_cmd(hDataFifo[WRITER_INDEX], DATAFIFO_CMD_GET_AVAIL_WRITE_LEN, &availWriteLen);
        if (K_SUCCESS != s32Ret)
        {
            printf("get available write len error:%x\n", s32Ret);
            //break;
        }
        else if (availWriteLen >= DATAFIFO_BLOCK_LEN)
        {
            printf("About to send...\n");
            //memset(buf, 0, DATAFIFO_BLOCK_LEN);

            FILE *file = fopen("pic_nv12_quarter.raw", "rb");
            if (file == NULL)
            {
                printf("%s failed to open pic.jpg\n", __func__);
            }
            size_t bytes_read = fread(buf, 1, DATAFIFO_BLOCK_LEN, file);
            fclose(file);


            s32Ret = kd_datafifo_write(hDataFifo[WRITER_INDEX], buf);
            if (K_SUCCESS != s32Ret)
            {
                printf("write error:%x\n", s32Ret);
                //break;
            }

            s32Ret = kd_datafifo_cmd(hDataFifo[WRITER_INDEX], DATAFIFO_CMD_WRITE_DONE, NULL);
            if (K_SUCCESS != s32Ret)
            {
                printf("write done error:%x\n", s32Ret);
                //break;
            }

            MSG_CMD_DETECT_RGB_struct   msg {
                .width = 648,
                .height = 486,
            };
            auto pReq = kd_ipcmsg_create_message(0, MSG_CMD_DETECT_RGB, &msg, sizeof(MSG_CMD_DETECT_RGB_struct));
            k_ipcmsg_message_t *responce = nullptr;
            ret = kd_ipcmsg_send_sync(ipcmsg_handle, pReq, &responce, 2000);
            if (ret != K_SUCCESS) {
                printf("kd_ipcmsg_send_sync failed: %d\n", ret);
            }
            else if (responce->u32CMD == MSG_CMD_DETECT_RGB && responce->s32RetVal == K_SUCCESS) {
                printf("MSG_CMD_DETECT_RGB success, %lu\n", responce->u32BodyLen);
            }
            kd_ipcmsg_destroy_message(responce);
            kd_ipcmsg_destroy_message(pReq);
        }
        else
        {
            printf("no free space: %d\n", availWriteLen);
        }
    }

    asio::io_context io_context;
    asio::ip::udp::socket socket(io_context, asio::ip::udp::endpoint(asio::ip::udp::v4(), 5555));
    std::thread udp_receiver_thread(udp_receiver, &socket);

    std::thread read_fifo_thread(read_fifo, &socket);

    if (!daemon_mode) {
        printf("Input q to exit: \n");
        while (getchar() != 'q') {
            usleep(10000);
        }

        send_stop = true;
    }

    read_fifo_thread.join();

    socket.close();
    udp_receiver_thread.join();

    // datafifo反初始化
    datafifo_deinit();

    fclose(output_file_video);
    fclose(output_file_detections);

    kd_ipcmsg_disconnect(ipcmsg_handle);
    kd_ipcmsg_del_service(IPCMSG_NAME);
    ipcmsg_thread.join();

    return 0;
}