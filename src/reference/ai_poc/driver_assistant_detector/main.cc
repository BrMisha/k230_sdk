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

float overlap_ratio = 0;
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
    printf("release %p\n", pStream);
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
        // Write stream to h265 file

        for (i = 0; i < output.pack_cnt; i++) {
            k_u8 *pData;
            pData = (k_u8 *) kd_mpi_sys_mmap(output.pack[i].phys_addr, output.pack[i].len);
            printf("venc_output... size %lu, availWriteLen %lu\n", output.pack[i].len, availWriteLen);

            if (availWriteLen >= DATAFIFO_DETECTOR_BLOCK_LEN) {
                size_t total_size = 0;

                if (output.pack[i].type != K_VENC_HEADER) {
                    std::vector<DetectionCommon> detections;
                    {
                        std::lock_guard<std::mutex> lock(last_detections_mutex);

                        if (last_detections.size() != 0) {
                            auto item = std::move(last_detections.front());
                            last_detections.pop();

                            if (item.pts == output.pack[i].pts) {
                                detections = std::move(item.detections);
                                //printf("last_detections_pts valid\n");
                            } else {
                                printf("last_detections_pts IS INVALID!!!!!!!!!!!!!!!!!!!! %lu %lu\n", item.pts,
                                       output.pack[i].pts);
                            }
                        }
                    }

                    uint16_t s = detections.size();
                    memcpy(datafifo_buf, &s, sizeof(s));
                    total_size = sizeof(s);
                    for (auto &it: detections) {
                        memcpy(datafifo_buf + total_size, &it, sizeof(DetectionCommon));
                        total_size += sizeof(DetectionCommon);
                    }
                } else {
                    uint16_t s = UINT16_MAX;
                    memcpy(datafifo_buf, &s, sizeof(s));
                    total_size = sizeof(s);
                }

                memcpy(datafifo_buf + total_size, (void *) &(output.pack[i].pts), sizeof(k_u64));
                total_size += sizeof(k_u64);
                memcpy(datafifo_buf + total_size, (void *) &(output.pack[i].len), sizeof(k_u32));
                total_size += sizeof(k_u32);
                memcpy(datafifo_buf + total_size, (void *) pData, output.pack[i].len);
                total_size += output.pack[i].len;

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

void rgb_reduce_size(cv::Mat &rgb_frame, int max_width) {
    if (rgb_frame.cols <= max_width) return;

    float scale = static_cast<float>(max_width) / rgb_frame.cols;
    int new_width = max_width;
    int new_height = static_cast<int>(rgb_frame.rows * scale);
    cv::resize(rgb_frame, rgb_frame, cv::Size(new_width, new_height));
    rgb_frame.cols = new_width;
    rgb_frame.rows = new_height;
}

// Receive picture from camera.
// Push to encoder.
// Resizes to max_width and put to buffer
void isp_poll(Media *media, int debug_mode, int detection_max_width) {
    k_u64 time_pts = 0;
    uint8_t *rgb_buffer = static_cast<uint8_t *>(malloc(static_cast<size_t>(media->input_config()->sensor_width) *
        static_cast<size_t>(media->input_config()->sensor_height) * 3));

    while (running) {
        ScopedTiming st("----- isp_poll total", 1);

        k_video_frame_info dump_info;
        {
            ScopedTiming st("isp_dump", 1);
            auto picture = media->isp_dump(dump_info);
            if (!picture) {
                printf("!!!!!!!!! ISP DUMP !!!!!!!!\n");
                break;
            }

            auto vbvaddr = picture.value()->vbvaddr();

            // convert to rgb and save to rgb
            Utils::nv12ToRGBHWC(reinterpret_cast<uint8_t*>(vbvaddr),
                media->input_config()->sensor_width, media->input_config()->sensor_height, rgb_buffer);
        }
        // Now the rgb_buffer contains image and camera buffer released

        cv::Mat rgb_frame(dump_info.v_frame.height, dump_info.v_frame.width, CV_8UC3, rgb_buffer);

        {
            ScopedTiming st("RGB to encoder", debug_mode);

            // Convert RGB image to ARGB image, send to encoder
            uint8_t *src = rgb_frame.data;
            uint8_t *dst = static_cast<uint8_t *>(media->venc_get_pic_vaddr());

            for (int y = 0; y < rgb_frame.rows; y++) {
                for (int x = 0; x < rgb_frame.cols; x++) {
                    int src_idx = (y * rgb_frame.cols + x) * 3;
                    int dst_idx = (y * rgb_frame.cols + x) * 4;

                    // Copy RGB values and add alpha channel (255 = fully opaque)
                    dst[dst_idx + 0] = 255; // Alpha
                    dst[dst_idx + 1] = src[src_idx + 2]; // B
                    dst[dst_idx + 2] = src[src_idx + 1]; // G
                    dst[dst_idx + 3] = src[src_idx + 0]; // R
                }
            }

            media->venc_push(time_pts);
        }

        if (rgb_frame.cols > detection_max_width) {
            ScopedTiming st("Image resize", 1);
            rgb_reduce_size(rgb_frame, detection_max_width);
            std::cout << "Resized image to: " << rgb_frame.cols << "x" << rgb_frame.rows << std::endl;
        }


        time_pts++;
    }

    free(rgb_buffer);
}

std::vector<DetectionNormalized> detect(SAHI &sahi, cv::Mat &rgb_frame, int detection_max_width) {
    std::vector<DetectionNormalized> results;

    if (rgb_frame.cols > detection_max_width) {
        ScopedTiming st("Image resize", 1);
        rgb_reduce_size(rgb_frame, detection_max_width);
        std::cout << "Resized image to: " << rgb_frame.cols << "x" << rgb_frame.rows << std::endl;
    }

    auto r = sahi.detect(rgb_frame);
    for (auto it = r.cbegin(); it != r.cend(); ++it) {
        results.push_back(it->normalize(rgb_frame.cols, rgb_frame.rows));
    }

    return results;
}

void isp_ai_detector(Media *media, int debug_mode, char *fd_kmodel_path, float facedet_obj_thresh, float facedet_nms_thresh,
                     float overlap_ratio, int detection_max_width) {

    struct Buffer {
        uint8_t *rgb;
        //uint8_t *argb;
        k_u32   width{};
        k_u32   height{};
        std::mutex mutex;
        std::condition_variable cv;

        Buffer(size_t _width, size_t _height) {
            width = _width;
            height = _height;

            rgb = static_cast<uint8_t *>(malloc(width * height * 3));
        }
        ~Buffer() {
            free(rgb);
        }
    };

    k_u64 time_pts = 0;

    printf("start loop\n");

    Buffer buffer = Buffer(media->input_config()->sensor_width, media->input_config()->sensor_height);

    std::thread camera_receiver_thread([&]() {
        while (running) {
            k_video_frame_info dump_info;
            int ret;
            auto picture = media->isp_dump(dump_info);
            if (!picture) {
                printf("!!!!!!!!! ISP DUMP !!!!!!!!. Error: %d\n", ret);
                break;
            }

            auto vbvaddr = picture.value()->vbvaddr();

            std::unique_lock<std::mutex> lock(buffer.mutex, std::try_to_lock);
            // if unable to lock, this frame will droped (we need only last frame for minimal latency)
            if (lock.owns_lock()) {
                buffer.width = dump_info.v_frame.width;
                buffer.height = dump_info.v_frame.height;

                // convert to rgb and save to buffer.rgb
                Utils::nv12ToRGBHWC(reinterpret_cast<uint8_t*>(vbvaddr),
                    media->input_config()->sensor_width, media->input_config()->sensor_height, buffer.rgb);

                buffer.cv.notify_one();
            }
        }

        buffer.cv.notify_one();
    });

    while (running) {
        ScopedTiming st("----------------Total time--------------- " + std::to_string(time_pts), 1);

        std::unique_lock<std::mutex> lock(buffer.mutex);
        buffer.cv.wait(lock);
        cv::Mat rgb_frame(buffer.height, buffer.width, CV_8UC3, buffer.rgb);
        if (buffer.width == 0 || buffer.height == 0) continue;

        // Copy to encoder because the rgb_frame may resized on next step
        {
            ScopedTiming st("RGB to ARGB", debug_mode);

            // Convert RGB image to ARGB image, send to encoder
            uint8_t *src = rgb_frame.data;
            uint8_t *dst = (uint8_t *) media->venc_get_pic_vaddr();

            for (int y = 0; y < rgb_frame.rows; y++) {
                for (int x = 0; x < rgb_frame.cols; x++) {
                    int src_idx = (y * rgb_frame.cols + x) * 3;
                    int dst_idx = (y * rgb_frame.cols + x) * 4;

                    // Copy RGB values and add alpha channel (255 = fully opaque)
                    dst[dst_idx + 0] = 255; // Alpha
                    dst[dst_idx + 1] = src[src_idx + 2]; // B
                    dst[dst_idx + 2] = src[src_idx + 1]; // G
                    dst[dst_idx + 3] = src[src_idx + 0]; // R
                }
            }
        }



        //cv::Mat rgb_frame = Utils::nv12ToRGBHWC((uint8_t *) vbvaddr, media->input_config()->sensor_width, media->input_config()->sensor_height, rgb_buffer);

        std::vector<DetectionNormalized> results;

        {
            ScopedTiming st("SAHI detection", 1);
            std::lock_guard<std::mutex> lock(obDet_mutex);
            SAHI sahi(obDet, cv::Size(320, 320), overlap_ratio);
            results = detect(sahi, rgb_frame, detection_max_width);
        }

        {
            ScopedTiming st("osd draw", debug_mode);

            for (int i = 0; i < results.size(); ++i) {
                const auto &det = results[i];
                auto d = Detection::from_normalized(det, buffer.width, buffer.height);
                std::cout << "Object " << (i + 1) << ": "
                        << detect_classes[d.class_id] << " (ID:" << d.class_id << ") "
                        << "confidence=" << d.confidence << " "
                        << "box=[" << d.box.x << "," << d.box.y << ","
                        << d.box.width << "x" << d.box.height << "]"
                        << std::endl;
            }
        }

        {
            ScopedTiming st("venc_send_frame", debug_mode);

            {
                last_detection_t ld;
                ld.pts = time_pts;

                for (auto it = results.cbegin(); it != results.cend(); ++it) {
                    auto d = Detection::from_normalized(*it, buffer.width, buffer.height);

                    DetectionCommon dc;
                    memset(&dc, 0, sizeof(DetectionCommon));
                    dc.class_id = d.class_id;
                    dc.confidence = d.confidence;

                    dc.x = static_cast<uint16_t>(d.box.x);
                    dc.y = static_cast<uint16_t>(d.box.y);
                    dc.w = static_cast<uint16_t>(d.box.width);
                    dc.h = static_cast<uint16_t>(d.box.height);

                    ld.detections.push_back(dc);
                }

                std::lock_guard<std::mutex> lock(last_detections_mutex);
                last_detections.push(ld);
            }

            media->venc_push(time_pts++);
        }
    }

    camera_receiver_thread.join();
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

                    static void *rgb_buffer = nullptr;
                    if (rgb_buffer == nullptr) {
                        rgb_buffer = malloc(2592 * 2048 * 3);
                    }

                    cv::Mat rgb_frame = Utils::nv12ToRGBHWC(pBuf, data->width, data->height, reinterpret_cast<uint8_t*>(rgb_buffer));

                    s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_READ_DONE, pBuf);
                    if (K_SUCCESS != s32Ret) {
                        printf("fifo_reader_thread read done error:%x\n", s32Ret);
                        break;
                    }

                    std::lock_guard<std::mutex> lock(obDet_mutex);
                    SAHI sahi(obDet, cv::Size(320, 320), overlap_ratio);
                    auto results = detect(sahi, rgb_frame, 5000);
                    printf("Detected count: %lu\n", results.size());
                    static_cast<uint8_t*>(rgb_buffer)[0] = static_cast<uint8_t>(results.size());

                    for (size_t i = 0; i < results.size(); ++i) {
                        auto d = Detection::from_normalized(results[i], data->width, data->height);

                        DetectionCommon dc;
                        memset(&dc, 0, sizeof(DetectionCommon));
                        dc.class_id = d.class_id;
                        dc.confidence = d.confidence;

                        dc.x = static_cast<uint16_t>(d.box.x);
                        dc.y = static_cast<uint16_t>(d.box.y);
                        dc.w = static_cast<uint16_t>(d.box.width);
                        dc.h = static_cast<uint16_t>(d.box.height);

                        memcpy(rgb_buffer + sizeof(uint8_t) + (i * sizeof(DetectionCommon)), &dc, sizeof(DetectionCommon));
                    }
                    pResp = kd_ipcmsg_create_resp_message(msg, K_SUCCESS, rgb_buffer, sizeof(uint8_t) + results.size() * sizeof(DetectionCommon));

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
    cout << "Usage: " << name << " <debug_mode> <image_input_mode> <kmodel> <obj_thresh> <nms_thresh> <overlap_ratio> <detection_max_width>" << endl
            << "For example: " << endl
            << " ./driver_assistant_detector.elf 0 0 yolov8n.kmodel 0.5 0.45 0.2 1920" << endl
            << "Options:" << endl
            << " 1> debug_mode           Debug mode: 0=no debug, 1=simple debug, 2=detailed debug\n"
            << " 2> image_input_mode     Image input mode\n"
            << " 3> kmodel               Object detection kmodel file path\n"
            << " 4> obj_thresh           Object detection threshold\n"
            << " 5> nms_thresh           NMS threshold\n"
            << " 6> overlap_ratio        SAHI overlap ratio (e.g., 0.2)\n"
            << " 7> detection_max_width  Maximum width for detection (image will be resized if larger)\n"
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
    float facedet_obj_thresh = atof(argv[4]);
    float facedet_nms_thresh = atof(argv[5]);
    overlap_ratio = atof(argv[6]);
    int detection_max_width = atoi(argv[7]);

    gpio_led_fd = open("/dev/gpio", O_RDWR);
    pin_mode_t mode;
    mode.pin = LED_PIN_NUM;
    ioctl(gpio_led_fd, GPIO_DM_OUTPUT, &mode);
    ioctl(gpio_led_fd, GPIO_WRITE_LOW, &mode);

    obDet = new OBDet(fd_kmodel_path, facedet_obj_thresh, facedet_nms_thresh, 0);

    // datafifo
    k_s32 ret = datafifo_init();
    if (0 != ret) {
        std::cout << "====== datafifo init failed ======";
    }

    k_s32 ipcmsg_handle;
    /*{
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
    }*/
    std::thread ipcmsg_thread([ipcmsg_handle] {
        //kd_ipcmsg_run(ipcmsg_handle);
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
            .bitrate_kbps = 4000
        };
        Media media(config);
        media.init();

        std::thread isp_poll_thread(isp_poll, &media, debug_mode, detection_max_width);

        /*std::thread isp_ai_detector_thread(isp_ai_detector, &media, debug_mode, fd_kmodel_path, facedet_obj_thresh,
                                           facedet_nms_thresh, overlap_ratio, detection_max_width);*/

        std::thread venc_output_thread(venc_output, media.venc_get_channel());

        while (getchar() != 'q') {
            usleep(10000);
        }
        running = false;

        isp_poll_thread.join();
        venc_output_thread.join();
    }

    kd_ipcmsg_disconnect(ipcmsg_handle);
    kd_ipcmsg_del_service(IPCMSG_NAME);
    ipcmsg_thread.join();

    // datafifo exit
    datafifo_deinit();

    return 0;
}
