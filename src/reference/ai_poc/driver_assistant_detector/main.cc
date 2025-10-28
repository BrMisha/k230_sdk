#include <condition_variable>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <thread>
#include <sys/types.h>
#include <fcntl.h>
#include <future>
#include <sys/ioctl.h>
#include "utils.h"
#include <opencv2/opencv.hpp>
#include <mutex>
#include <memory>
#include <iomanip>

#include "k_ipcmsg.h"
#include "k_module.h"
#include "k_type.h"
#include "k_vb_comm.h"
#include "k_video_comm.h"
#include "k_sys_comm.h"
#include "mpi_vb_api.h"
#include "mpi_vo_api.h"
#include "mpi_sys_api.h"
#include "k_vvo_comm.h"
#include "mpi_venc_api.h"
#include "k_venc_comm.h"
#include "mpi_vvi_api.h"
#include "vo_test_case.h"
#include "sahi.h"

#include "k_datafifo.h"
#include "media.h"
#include "image_decoder.h"

#include "common.h"

// GPIO userspace definitions (from sample_gpio.c)
#define GPIO_DM_OUTPUT           _IOW('G', 0, int)
#define GPIO_DM_INPUT            _IOW('G', 1, int)
#define GPIO_DM_INPUT_PULL_UP    _IOW('G', 2, int)
#define GPIO_DM_INPUT_PULL_DOWN  _IOW('G', 3, int)
#define GPIO_WRITE_LOW           _IOW('G', 4, int)
#define GPIO_WRITE_HIGH          _IOW('G', 5, int)
#define LED_PIN_NUM 52

typedef struct {
    unsigned short pin;     /* pin number, from 0 to 63 */
    unsigned short mode;    /* pin level status, 0 low level, 1 high level */
} pin_mode_t;

// datafifo
#define READER_INDEX    0
#define WRITER_INDEX    1

static k_datafifo_handle hDataFifo[2] = {
    (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE, (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE
};
k_u64 datafifo_phy_addr[2] = {0,0};

std::atomic<bool> running(true);

struct last_detection_t {
    std::vector<DetectionCommon> detections;
    k_u64 pts;
};

std::mutex last_detections_mutex;
std::queue<last_detection_t> last_detections;

float sahi_overlap_ratio = 0;
float sahi_nms_threshold = 0;
std::mutex obDet_mutex;
OBDet *obDet;

int gpio_led_fd = 0;

//****************function***********************************

static inline void CHECK_RET(k_s32 ret, const char *func, const int line) {
    if (ret)
        printf("error ret %d, func %s line %d\n", ret, func, line);
}


// datafifo

static void release(void *pStream) {
    //printf("release %p\n", pStream);
}

static int datafifo_init(void) {
    k_s32 s32Ret = K_SUCCESS;

    k_datafifo_params_s writer_params = {10, DATAFIFO_DETECTOR_BLOCK_LEN, K_TRUE, DATAFIFO_WRITER};
    s32Ret = kd_datafifo_open(&hDataFifo[WRITER_INDEX], &writer_params);
    if (K_SUCCESS != s32Ret) {
        printf("open datafifo error:%x\n", s32Ret);
        return -1;
    }

    s32Ret = kd_datafifo_cmd(hDataFifo[WRITER_INDEX], DATAFIFO_CMD_GET_PHY_ADDR, &datafifo_phy_addr[WRITER_INDEX]);
    if (K_SUCCESS != s32Ret) {
        printf("get datafifo phy addr error:%x\n", s32Ret);
        return -1;
    }
    printf("PhyAddr writer: %lx\n", datafifo_phy_addr[WRITER_INDEX]);

    s32Ret = kd_datafifo_cmd(hDataFifo[WRITER_INDEX], DATAFIFO_CMD_SET_DATA_RELEASE_CALLBACK, (void *) release);

    if (K_SUCCESS != s32Ret) {
        printf("set release func callback error:%x\n", s32Ret);
        return -1;
    }

    k_datafifo_params_s reader_params = {2, DATAFIFO_FRONT_BLOCK_LEN, K_TRUE, DATAFIFO_READER};
    s32Ret = kd_datafifo_open(&hDataFifo[READER_INDEX], &reader_params);
    if (K_SUCCESS != s32Ret)
    {
        printf("open datafifo error:%x\n", s32Ret);
        return -1;
    }

    s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_GET_PHY_ADDR, &datafifo_phy_addr[READER_INDEX]);
    if (K_SUCCESS != s32Ret)
    {
        printf("get datafifo phy addr error:%x\n", s32Ret);
        return -1;
    }
    printf("PhyAddr reader: %lx\n", datafifo_phy_addr[READER_INDEX]);


    printf("datafifo_init finish\n");

    return 0;
}

void datafifo_deinit(void) {
    k_s32 s32Ret = K_SUCCESS;
    // call write NULL to flush and release stream buffer.
    s32Ret = kd_datafifo_write(hDataFifo[WRITER_INDEX], NULL);
    if (K_SUCCESS != s32Ret) {
        printf("write error:%x\n", s32Ret);
    }
    printf(" kd_datafifo_close %lx\n", hDataFifo[WRITER_INDEX]);
    printf(" kd_datafifo_close %lx\n", hDataFifo[READER_INDEX]);
    kd_datafifo_close(hDataFifo[WRITER_INDEX]);
    kd_datafifo_close(hDataFifo[READER_INDEX]);
    printf(" finish\n");
}


/**
* Encoder output thread logic
*/
static void venc_output(k_u32 venc_ch) {
    k_char *datafifo_buf = (k_char *) malloc(DATAFIFO_DETECTOR_BLOCK_LEN);
    memset(datafifo_buf, 0, DATAFIFO_DETECTOR_BLOCK_LEN);

    k_venc_stream output;
    k_s32 ret;
    int i;

    printf("venc_output... started\n");

    while (running) {
        // datafifo
        k_u32 availWriteLen = 0;
        // call write NULL to flush
        ret = kd_datafifo_write(hDataFifo[WRITER_INDEX], NULL);
        if (K_SUCCESS != ret) {
            printf("venc_output...write error:%x\n", ret);
        }
        ret = kd_datafifo_cmd(hDataFifo[WRITER_INDEX], DATAFIFO_CMD_GET_AVAIL_WRITE_LEN, &availWriteLen);
        if (K_SUCCESS != ret) {
            printf("venc_output...get available write len error:%x\n", ret);
            break;
        }

        k_venc_chn_status status;
        ret = kd_mpi_venc_query_status(venc_ch, &status);
        CHECK_RET(ret, __func__, __LINE__);

        if (status.cur_packs > 0)
            output.pack_cnt = status.cur_packs;
        else
            output.pack_cnt = 1;
        output.pack = static_cast<k_venc_pack *>(malloc(sizeof(k_venc_pack) * output.pack_cnt));

        // // Set keyframe frequency
        // if (index % 4 == 0)
        // {
        //     index = 0;
        //     ret = kd_mpi_venc_request_idr(0);
        // }
        // index ++;

        // Get encoded stream
        ret = kd_mpi_venc_get_stream(venc_ch, &output, -1);
        CHECK_RET(ret, __func__, __LINE__);

        for (i = 0; i < output.pack_cnt; i++) {
            k_u8 *pData;
            pData = (k_u8 *) kd_mpi_sys_mmap(output.pack[i].phys_addr, output.pack[i].len);
            //printf("venc_output... size %lu, type %d, pts %lu\n", output.pack[i].len, output.pack[i].type, output.pack[i].pts);

            if (availWriteLen >= DATAFIFO_DETECTOR_BLOCK_LEN) {
                auto dff = reinterpret_cast<DataFifoFrame_t *>(datafifo_buf);
                dff->type = output.pack[i].type;
                dff->pts = output.pack[i].pts;
                dff->data_len = output.pack[i].len;
                if (DATAFIFO_DETECTOR_BLOCK_LEN >= sizeof(DataFifoFrame_t) + dff->data_len) {
                    memcpy(dff->data, static_cast<void *>(pData), dff->data_len);
                } else {
                    printf("data fifo size IS INVALID %lu !!!!!!!!!!!!!!!!!!!\n", sizeof(DataFifoFrame_t) + dff->data_len);
                }

                ret = kd_datafifo_write(hDataFifo[WRITER_INDEX], datafifo_buf);
                if (K_SUCCESS != ret) {
                    printf("venc_output...write error:%x\n", ret);
                    break;
                }
                ret = kd_datafifo_cmd(hDataFifo[WRITER_INDEX], DATAFIFO_CMD_WRITE_DONE, NULL);
                if (K_SUCCESS != ret) {
                    printf("venc_output...write done error:%x\n", ret);
                    break;
                }
            }

            kd_mpi_sys_munmap(pData, output.pack[i].len);
        }

        ret = kd_mpi_venc_release_stream(venc_ch, &output);
        CHECK_RET(ret, __func__, __LINE__);

        free(output.pack);
    }

    free(datafifo_buf);
}

std::vector<DetectionNormalized> detect(SAHI &sahi, cv::Mat &rgb_frame) {
    std::vector<DetectionNormalized> results;

    auto r = sahi.detect(rgb_frame);
    for (auto it = r.cbegin(); it != r.cend(); ++it) {
        results.push_back(it->normalize(rgb_frame.cols, rgb_frame.rows));
    }

    return results;
}

void isp_ai_detector(Media *media, int debug_mode, float overlap_ratio, k_s32 ipcmsg_handle) {

    std::vector<DetectionNormalized> results_to_push;
    uint64_t results_to_push_pts = UINT64_MAX;
    std::mutex results_to_push_mutex;
    std::condition_variable results_to_push_cv;

    std::thread push_thread([&]() {
        static const size_t MAX_COUNT = 50;
        auto *buf = static_cast<uint8_t *>(malloc(  sizeof(DetectionNormalizedCommon) * MAX_COUNT + sizeof(results_to_push_pts)));
        while (ipcmsg_handle && running) {
            std::unique_lock<std::mutex> lock(results_to_push_mutex);
            if (results_to_push_pts == UINT64_MAX) {
                if (running)
                    results_to_push_cv.wait(lock);
            }
            else {
                printf("Detections count: %lu, pts: %lu\n", results_to_push.size(), results_to_push_pts);
                for (int i = 0; i < results_to_push.size(); ++i) {
                    const auto &det = results_to_push[i];
                    //auto d = Detection::from_normalized(det, rgb_frame->cols, rgb_frame->rows);
                    std::cout << "Object " << (i + 1) << ": "
                            << detect_classes[det.class_id] << " (ID:" << det.class_id << ") "
                            << "confidence=" << det.confidence << " "
                            << "box=[" << det.box.x << "," << det.box.y << ","
                            << det.box.width << "x" << det.box.height << "]"
                            << std::endl;
                }

                *reinterpret_cast<uint64_t*>(buf) = results_to_push_pts;
                for (size_t i = 0; i < results_to_push.size() && i < MAX_COUNT;  ++i) {
                    auto dnc = &reinterpret_cast<DetectionNormalizedCommon*>(buf + sizeof(results_to_push_pts))[i];
                    dnc->class_id = results_to_push[i].class_id;
                    dnc->confidence = results_to_push[i].confidence;
                    dnc->x = results_to_push[i].box.x;
                    dnc->y = results_to_push[i].box.y;
                    dnc->w = results_to_push[i].box.width;
                    dnc->h = results_to_push[i].box.height;
                }
                auto pReq = kd_ipcmsg_create_message(0, MSG_CMD_DETECTIONS, buf,
                    sizeof(results_to_push_pts) + (sizeof(DetectionNormalizedCommon) * results_to_push.size()));
                auto ret = kd_ipcmsg_send_only(ipcmsg_handle, pReq);
                kd_ipcmsg_destroy_message(pReq);

                results_to_push_pts = UINT64_MAX;
            }
        }
        free(buf);
    });

    std::unique_ptr<cv::Mat> rgb_frame;
    while (running) {
        ScopedTiming st("----------------Total time--------------- ", 1);

        k_video_frame_info dump_info;
        int ret;
        {
            ScopedTiming st_isp_dump_rgb888("isp_dump_rgb888", debug_mode);
            auto picture = media->isp_dump_rgb888(dump_info, 1);
            if (!picture) {
                printf("!!!!!!!!! ISP DUMP !!!!!!!!. Error: %d\n", ret);
                break;
            }

            if (!rgb_frame)
                rgb_frame = std::make_unique<cv::Mat>(dump_info.v_frame.height, dump_info.v_frame.width, CV_8UC3);

            // Copy camera RGB data to buffer. We can not use vbvaddr directly for detection because it is to low
            memcpy(rgb_frame->data, picture.value()->vbvaddr(), rgb_frame->cols * rgb_frame->rows * 3);
        }

        if (!rgb_frame) continue;

        std::vector<DetectionNormalized> results;

        {
            ScopedTiming st("SAHI detection", 1);
            std::lock_guard<std::mutex> lock(obDet_mutex);
            SAHI sahi(obDet, cv::Size(320, 320), overlap_ratio, sahi_nms_threshold);
            results = detect(sahi, *rgb_frame);
            printf("Detections count: %lu\n", results.size());
        }

        std::lock_guard<std::mutex> lock(results_to_push_mutex);
        results_to_push_pts = dump_info.v_frame.pts;
        results_to_push = std::move(results);
        results_to_push_cv.notify_all();
    }

    results_to_push_cv.notify_all();
    push_thread.join();
}

static void ipcmsg_recv(k_s32 s32Id, k_ipcmsg_message_t *msg) {
    printf("ipcmsg_recv %lu\n", msg->u32CMD);
    switch (msg->u32CMD) {
        case MSG_CMD_GET_PHY_ADDRESS: {
            auto pResp = kd_ipcmsg_create_resp_message(msg, K_SUCCESS, datafifo_phy_addr, sizeof(datafifo_phy_addr));
            kd_ipcmsg_send_only(s32Id, pResp);
            kd_ipcmsg_destroy_message(pResp);
        } break;
        case MSG_CMD_DETECT_RGB: {
            ScopedTiming st("MSG_CMD_DETECT_RGB", 1);

            k_ipcmsg_message_t  *pResp = nullptr;
            if (msg->u32BodyLen != sizeof(MSG_CMD_DETECT_RGB_struct)) {
                printf("MSG_CMD_DETECT_RGB. Wrong len of header: %lu\n", msg->u32BodyLen);
                pResp = kd_ipcmsg_create_resp_message(msg, K_FAILED, nullptr, 0);
            }
            else {
                auto data = reinterpret_cast<MSG_CMD_DETECT_RGB_struct*>(msg->pBody);

                k_u32 readLen = 0;
                k_s32 s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_GET_AVAIL_READ_LEN, &readLen);
                if (K_SUCCESS != s32Ret) {
                    printf("fifo_reader_thread get available read len error:%x\n", s32Ret);
                    break;
                }

                if (readLen > 0) {
                    uint8_t* pBuf;
                    s32Ret = kd_datafifo_read(hDataFifo[READER_INDEX], (void**)&pBuf);
                    if (K_SUCCESS != s32Ret) {
                        printf("kd_datafifo_read read error: %x\n", s32Ret);
                        break;
                    }

                    // We need to clone because detection works to slow if image in DATAFIFO.
                    // To slow - it is an additional 500 ms
                    auto rgb_frame = cv::Mat(data->height, data->width, CV_8UC3, pBuf).clone();

                    s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_READ_DONE, pBuf);
                    if (K_SUCCESS != s32Ret) {
                        printf("fifo_reader_thread read done error:%x\n", s32Ret);
                        break;
                    }

                    std::lock_guard lock(obDet_mutex);
                    SAHI sahi(obDet, cv::Size(320, 320), sahi_overlap_ratio, sahi_nms_threshold);
                    auto results = detect(sahi, rgb_frame);
                    printf("Detected count: %lu\n", results.size());

                    // We don't need the rgb_frame anymore and will use allocated memory just as buffer for responce
                    uint8_t *response_buf = rgb_frame.data;

                    response_buf[0] = static_cast<uint8_t>(results.size());
                    for (size_t i = 0; i < results.size(); ++i) {
                        auto &d = results[i];

                        DetectionNormalizedCommon dc;
                        memset(&dc, 0, sizeof(DetectionNormalizedCommon));
                        dc.class_id = d.class_id;
                        dc.confidence = d.confidence;

                        dc.x = d.box.x;
                        dc.y = d.box.y;
                        dc.w = d.box.width;
                        dc.h = d.box.height;

                        memcpy( response_buf + sizeof(uint8_t) + (i * sizeof(DetectionNormalizedCommon)), &dc, sizeof(DetectionNormalizedCommon));
                    }
                    pResp = kd_ipcmsg_create_resp_message(msg, K_SUCCESS, response_buf, sizeof(uint8_t) + results.size() * sizeof(DetectionNormalizedCommon));

                }
                else {
                    pResp = kd_ipcmsg_create_resp_message(msg, K_FAILED, nullptr, 0);
                }

            }

            if (pResp) {
                printf("Send responce\n");
                kd_ipcmsg_send_only(s32Id, pResp);
                kd_ipcmsg_destroy_message(pResp);
            }
        } break;
        case MSG_CMD_LED_SET: {
            if (msg->u32BodyLen == sizeof(uint8_t)) {
                auto state = *static_cast<uint8_t *>(msg->pBody);

                pin_mode_t mode;
                mode.pin = LED_PIN_NUM;
                ioctl(gpio_led_fd, GPIO_DM_OUTPUT, &mode);
                ioctl(gpio_led_fd, state ? GPIO_WRITE_HIGH : GPIO_WRITE_LOW, &mode);
            }
        } break;
        default:
            break;
    }
}

void print_usage(const char *name) {
    cout << "Usage: " << name << " <debug_mode> <image_input_mode> <kmodel> <obj_thresh> <nms_thresh> <sahi_nms_thresh> <overlap_ratio>" << endl
            << "For example: " << endl
            << " ./driver_assistant_detector.elf 0 0 yolov8n.kmodel 0.5 0.45 0.35 0.2" << endl
            << "Options:" << endl
            << " 1> debug_mode           Debug mode: 0=no debug, 1=simple debug, 2=detailed debug\n"
            << " 2> image_input_mode     Image input mode\n"
            << " 3> kmodel               Object detection kmodel file path\n"
            << " 4> obj_thresh           Object detection threshold\n"
            << " 5> nms_thresh           Per-tile NMS threshold (e.g., 0.45)\n"
            << " 6> sahi_nms_thresh      SAHI global NMS threshold (e.g., 0.35)\n"
            << " 7> overlap_ratio        SAHI overlap ratio (e.g., 0.2)\n"
            << "\n"
            << endl;
}

int main(int argc, char *argv[]) {
    std::cout << "case " << argv[0] << " built at " << __DATE__ << " " << __TIME__ << std::endl;
    if (argc != 8) {
        print_usage(argv[0]);
        return -1;
    }

    int debug_mode = atoi(argv[1]);
    int image_input_mode = atoi(argv[2]);
    char *fd_kmodel_path = argv[3];
    float obj_det_thresh = atof(argv[4]);
    float obj_det_nms_thresh = atof(argv[5]);
    sahi_nms_threshold = atof(argv[6]);
    sahi_overlap_ratio = atof(argv[7]);

    // Print parsed parameters
    std::cout << "=== Parsed Parameters ===" << std::endl;
    std::cout << "  debug_mode:          " << (debug_mode ? "yes" : "no") << std::endl;
    std::cout << "  image_input_mode:    " << (image_input_mode ? "yes" : "no") << std::endl;
    std::cout << "  kmodel:              " << fd_kmodel_path << std::endl;
    std::cout << "  obj_det_thresh:      " << std::fixed << std::setprecision(2) << obj_det_thresh << std::endl;
    std::cout << "  obj_det_nms_thresh:  " << std::fixed << std::setprecision(2) << obj_det_nms_thresh << std::endl;
    std::cout << "  sahi_nms_threshold:  " << std::fixed << std::setprecision(2) << sahi_nms_threshold << std::endl;
    std::cout << "  sahi_overlap_ratio:  " << std::fixed << std::setprecision(2) << sahi_overlap_ratio << std::endl;
    std::cout << "=========================" << std::endl;

    gpio_led_fd = open("/dev/gpio", O_RDWR);
    pin_mode_t mode;
    mode.pin = LED_PIN_NUM;
    ioctl(gpio_led_fd, GPIO_DM_OUTPUT, &mode);
    ioctl(gpio_led_fd, GPIO_WRITE_LOW, &mode);

    obDet = new OBDet(fd_kmodel_path, obj_det_thresh, obj_det_nms_thresh, 0);

    // datafifo
    k_s32 ret = datafifo_init();
    if (0 != ret) {
        std::cout << "====== datafifo init failed ======";
    }

    k_s32 ipcmsg_handle = 0;
    {
        k_ipcmsg_connect_t stConnectAtt{
            .u32RemoteId = 0,
            .u32Port = 101,
            .u32Priority = 0
        };
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
    }
    std::thread ipcmsg_thread([ipcmsg_handle] {
        if (ipcmsg_handle) kd_ipcmsg_run(ipcmsg_handle);
    });

    if (image_input_mode) {
        while (getchar() != 'q') {
            usleep(10000);
        }
        running = false;
    }
    else {
        MediaInputConfig config {
            .sensor_width = 1920,
            .sensor_height = 1080,
            .rgb888_2_width = 768,
            .rgb888_2_height = 432,
            .bitrate_kbps = 4000
        };
        Media media(config);
        media.init();

        std::thread isp_ai_detector_thread(isp_ai_detector, &media, debug_mode, sahi_overlap_ratio, ipcmsg_handle);

        std::thread venc_output_thread(venc_output, media.venc_get_channel());

        while (getchar() != 'q') {
            usleep(10000);
        }
        running = false;

        isp_ai_detector_thread.join();
        venc_output_thread.join();
    }

    kd_ipcmsg_disconnect(ipcmsg_handle);
    kd_ipcmsg_del_service(IPCMSG_NAME);
    ipcmsg_thread.join();

    // datafifo exit
    datafifo_deinit();

    return 0;
}
