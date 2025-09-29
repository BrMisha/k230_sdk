/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/sysinfo.h>
#include "utils.h"
#include <opencv2/opencv.hpp>
#include <opencv2/highgui/highgui.hpp>
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

#include "vi_vo.h"
#include "k_datafifo.h"
#include "media.h"

#include "common.h"

#define ENABLE_VDEC_DEBUG    1
#define BIND_VO_LAYER   1

#ifdef ENABLE_VDEC_DEBUG
#define vdec_debug  printf
#else
#define vdec_debug(ARGS...)
#endif

#ifdef ENABLE_VDSS
#include "k_vdss_comm.h"
#include "mpi_vdss_api.h"
#else
#include "mpi_vicap_api.h"
#endif

#ifdef ENABLE_VENC_DEBUG
#define venc_debug  printf
#else
#define venc_debug(ARGS...)
#endif

#define VE_MAX_WIDTH ISP_CHN1_WIDTH
#define VE_MAX_HEIGHT ISP_CHN1_HEIGHT
#define VE_STREAM_BUF_SIZE ((VE_MAX_WIDTH*VE_MAX_HEIGHT/2 + 0xfff) & ~0xfff)
#define VE_FRAME_BUF_SIZE ((VE_MAX_WIDTH*VE_MAX_HEIGHT*2 + 0xfff) & ~0xfff)
#define VE_INPUT_BUF_CNT   6
#define VE_OUTPUT_BUF_CNT  15

// datafifo
#define READER_INDEX    0
#define WRITER_INDEX    1

static k_datafifo_handle hDataFifo[2] = {
    (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE, (k_datafifo_handle) K_DATAFIFO_INVALID_HANDLE
};
k_u64 datafifo_phy_addr[2] = {0,0};
static const k_s32 BLOCK_LEN = 102400;
k_char *datafifo_buf = (k_char *) malloc(BLOCK_LEN);

std::atomic<bool> isp_stop(false);

struct last_detection_t {
    std::vector<DetectionCommon> detections;
    k_u64 pts;
};

std::mutex last_detections_mutex;
std::queue<last_detection_t> last_detections;

//****************function***********************************

static inline void CHECK_RET(k_s32 ret, const char *func, const int line) {
    if (ret)
        printf("error ret %d, func %s line %d\n", ret, func, line);
}

/**
* VB initialization
*/
static k_s32 vb_init(k_u32 ch_cnt) {
    k_s32 ret;
    k_vb_config config;

    memset(&config, 0, sizeof(config));

    config.max_pool_cnt = 64;
    config.comm_pool[0].blk_cnt = VE_INPUT_BUF_CNT * ch_cnt;
    config.comm_pool[0].blk_size = VE_FRAME_BUF_SIZE;
    config.comm_pool[0].mode = VB_REMAP_MODE_NOCACHE;
    config.comm_pool[1].blk_cnt = VE_OUTPUT_BUF_CNT * ch_cnt;
    config.comm_pool[1].blk_size = VE_STREAM_BUF_SIZE;
    config.comm_pool[1].mode = VB_REMAP_MODE_NOCACHE;
    config.comm_pool[2].blk_cnt = 4;
    config.comm_pool[2].blk_size = (VE_MAX_WIDTH * VE_MAX_HEIGHT * 4);
    config.comm_pool[2].mode = VB_REMAP_MODE_NOCACHE;

    //VB for YUV420SP output
    config.comm_pool[3].blk_cnt = 5;
    config.comm_pool[3].mode = VB_REMAP_MODE_NOCACHE;
    config.comm_pool[3].blk_size = VICAP_ALIGN_UP((ISP_CHN0_WIDTH * ISP_CHN0_HEIGHT * SENSOR_CHANNEL) / 2,
                                                  VICAP_ALIGN_1K);

    //VB for RGB888 output
    config.comm_pool[4].blk_cnt = 5;
    config.comm_pool[4].mode = VB_REMAP_MODE_NOCACHE;
    config.comm_pool[4].blk_size = VICAP_ALIGN_UP((ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH * SENSOR_CHANNEL), VICAP_ALIGN_1K);

    ret = kd_mpi_vb_set_config(&config);

    k_vb_supplement_config supplement_config;
    memset(&supplement_config, 0, sizeof(supplement_config));
    supplement_config.supplement_config |= VB_SUPPLEMENT_JPEG_MASK;

    ret = kd_mpi_vb_set_supplement_config(&supplement_config);
    if (ret) {
        printf("vb_set_supplement_config failed ret:%d\n", ret);
        return ret;
    }

    venc_debug("-----------venc sample test------------------------\n");

    if (ret)
        venc_debug("vb_set_config failed ret:%d\n", ret);

    ret = kd_mpi_vb_init();
    if (ret)
        venc_debug("vb_init failed ret:%d\n", ret);

    return ret;
}

/**
* VB exit
*/
static k_s32 sample_vb_exit(void) {
    k_s32 ret;
    ret = kd_mpi_vb_exit();
    if (ret)
        vdec_debug("vb_exit failed ret:%d\n", ret);
    return ret;
}


// datafifo

static void release(void *pStream) {
    printf("release %p\n", pStream);
}

static int datafifo_init(void) {
    k_s32 s32Ret = K_SUCCESS;

    k_datafifo_params_s writer_params = {10, BLOCK_LEN, K_TRUE, DATAFIFO_WRITER};
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
    // s32Ret = kd_datafifo_cmd(hDataFifo[WRITER_INDEX], DATAFIFO_CMD_SET_DATA_RELEASE_CALLBACK, &phyAddr);

    if (K_SUCCESS != s32Ret) {
        printf("set release func callback error:%x\n", s32Ret);
        return -1;
    }

    k_datafifo_params_s reader_params = {10, BLOCK_LEN, K_TRUE, DATAFIFO_READER};
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
    // printf(" kd_datafifo_close %lx\n", hDataFifo[READER_INDEX]);
    kd_datafifo_close(hDataFifo[WRITER_INDEX]);
    kd_datafifo_close(hDataFifo[READER_INDEX]);
    printf(" finish\n");
}


/**
* Encoder output thread logic
*/
static void venc_output(k_u32 venc_ch) {
    memset(datafifo_buf, 0, BLOCK_LEN);
    k_venc_stream output;
    int out_cnt, out_frames;
    k_s32 ret;
    int i;
    k_u32 total_len = 0;

    out_cnt = 0;
    out_frames = 0;

    printf("venc_output... started\n");

    // int index = 0;
    while (!isp_stop) {
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
        out_cnt += output.pack_cnt;
        for (i = 0; i < output.pack_cnt; i++) {
            if (output.pack[i].type != K_VENC_HEADER) {
                out_frames++;
            }

            k_u8 *pData;
            pData = (k_u8 *) kd_mpi_sys_mmap(output.pack[i].phys_addr, output.pack[i].len);

            if (availWriteLen >= BLOCK_LEN) {
                size_t total_size = 0;

                if (output.pack[i].type != K_VENC_HEADER) {
                    std::vector<DetectionCommon> detections;
                    {
                        std::lock_guard<std::mutex> lock(last_detections_mutex);

                        //printf("last_detections %d, %lu\n", last_detections.size(), output.pack[i].pts);

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
                //printf("-----venc_output %lu %lu %d\n", output.pack[i].pts, detections.size(), output.pack[i].type);
                // copy detections into the buf


                memcpy(datafifo_buf + total_size, (void *) &(output.pack[i].pts), sizeof(k_u64));
                total_size += sizeof(k_u64);
                memcpy(datafifo_buf + total_size, (void *) &(output.pack[i].len), sizeof(k_u32));
                total_size += sizeof(k_u32);
                memcpy(datafifo_buf + total_size, (void *) pData, output.pack[i].len);
                total_size += output.pack[i].len;

                ret = kd_datafifo_write(hDataFifo[WRITER_INDEX], datafifo_buf);
                printf("venc_output... kd_datafifo_write %lu\n", ret);
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
            total_len += output.pack[i].len;
        }

        ret = kd_mpi_venc_release_stream(venc_ch, &output);
        CHECK_RET(ret, __func__, __LINE__);

        free(output.pack);
    }

    venc_debug("%s>done, ch %lu: out_frames %d, size %d bits\n", __func__, venc_ch, out_frames, total_len * 8);
}

/**
* Initialize frame to be sent to encoder after AI computation
*/
k_vb_blk_handle init_venc_frame(k_video_frame_info &vf_info, void **pic_vaddr, k_u32 g_pool_id) {
    k_u64 phys_addr = 0;
    k_u32 *virt_addr;
    k_vb_blk_handle handle;
    k_s32 size;

    if (vf_info.v_frame.pixel_format == PIXEL_FORMAT_ABGR_8888 || vf_info.v_frame.pixel_format ==
        PIXEL_FORMAT_ARGB_8888)
        size = vf_info.v_frame.height * vf_info.v_frame.width * 4;
    else if (vf_info.v_frame.pixel_format == PIXEL_FORMAT_RGB_565 || vf_info.v_frame.pixel_format ==
             PIXEL_FORMAT_BGR_565)
        size = vf_info.v_frame.height * vf_info.v_frame.width * 2;
    else if (vf_info.v_frame.pixel_format == PIXEL_FORMAT_ABGR_4444 || vf_info.v_frame.pixel_format ==
             PIXEL_FORMAT_ARGB_4444)
        size = vf_info.v_frame.height * vf_info.v_frame.width * 2;
    else if (vf_info.v_frame.pixel_format == PIXEL_FORMAT_RGB_888 || vf_info.v_frame.pixel_format ==
             PIXEL_FORMAT_BGR_888)
        size = vf_info.v_frame.height * vf_info.v_frame.width * 3;
    else if (vf_info.v_frame.pixel_format == PIXEL_FORMAT_ARGB_1555 || vf_info.v_frame.pixel_format ==
             PIXEL_FORMAT_ABGR_1555)
        size = vf_info.v_frame.height * vf_info.v_frame.width * 2;
    else if (vf_info.v_frame.pixel_format == PIXEL_FORMAT_YVU_PLANAR_420)
        size = vf_info.v_frame.height * vf_info.v_frame.width * 3 / 2;

    printf("vb block size is %x \n", size);

    handle = kd_mpi_vb_get_block(g_pool_id, size, NULL);
    if (handle == VB_INVALID_HANDLE) {
        printf("%s get vb block error\n", __func__);
        return K_FAILED;
    }

    phys_addr = kd_mpi_vb_handle_to_phyaddr(handle);
    if (phys_addr == 0) {
        printf("%s get phys addr error\n", __func__);
        return K_FAILED;
    }

    virt_addr = (k_u32 *) kd_mpi_sys_mmap(phys_addr, size);

    if (virt_addr == NULL) {
        printf("%s mmap error\n", __func__);
        return K_FAILED;
    }

    vf_info.mod_id = K_ID_VO;
    vf_info.pool_id = g_pool_id;
    vf_info.v_frame.phys_addr[0] = phys_addr;
    if (vf_info.v_frame.pixel_format == PIXEL_FORMAT_YVU_PLANAR_420)
        vf_info.v_frame.phys_addr[1] = phys_addr + (vf_info.v_frame.height * vf_info.v_frame.stride[0]);
    *pic_vaddr = virt_addr;

    printf("phys_addr is %lx g_pool_id is %d \n", phys_addr, g_pool_id);

    return handle;
}


std::vector<DetectionNormalized> detect(SAHI &sahi, cv::Mat &rgb_frame, int detection_max_width) {
    std::vector<DetectionNormalized> results;

    if (rgb_frame.cols > detection_max_width) {
        ScopedTiming st("Image resize", 1);
        float scale = static_cast<float>(detection_max_width) / rgb_frame.cols;
        int new_width = detection_max_width;
        int new_height = static_cast<int>(rgb_frame.rows * scale);
        cv::resize(rgb_frame, rgb_frame, cv::Size(new_width, new_height));
        rgb_frame.cols = new_width;
        rgb_frame.rows = new_height;
        std::cout << "Resized image to: " << rgb_frame.cols << "x" << rgb_frame.rows << std::endl;
    }
    auto r = sahi.detect(rgb_frame);
    for (auto it = r.cbegin(); it != r.cend(); ++it) {
        results.push_back(it->normalize(rgb_frame.cols, rgb_frame.rows));
    }

    return results;
}

void isp_ai_detector(int debug_mode, char *fd_kmodel_path, float facedet_obj_thresh, float facedet_nms_thresh,
                     float overlap_ratio, int detection_max_width) {
    int ret;

    while (1) {
        ret = vivcap_start();
        if (ret) {
            printf("ERROR vivcap_start %lu\n", ret);
            vivcap_stop();
        } else {
            sleep(2);
            memset(&dump_info, 0, sizeof(k_video_frame_info));
            ret = kd_mpi_vicap_dump_frame(vicap_dev, VICAP_CHN_ID_1, VICAP_DUMP_YUV, &dump_info, 1000);
            if (ret) {
                printf("ERROR kd_mpi_vicap_dump_frame %lu\n", ret);

                ret = kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
                if (ret) {
                    printf("ERROR kd_mpi_vicap_dump_release %lu\n", ret);
                }

                vivcap_stop();
            } else {
                kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
                break;
            }
        }
    }
    printf("vivcap done\n");

    size_t size = (SENSOR_CHANNEL * ISP_CHN1_HEIGHT * ISP_CHN1_WIDTH) / 2;

    OBDet obDet(fd_kmodel_path, facedet_obj_thresh, facedet_nms_thresh, 0);
    SAHI sahi(&obDet, cv::Size(320, 320), overlap_ratio);

    //******************* After AI computation, assemble results into k_video_frame_info frame object format *******************
    // Some initialization settings here, choose to use 1080P, ARGB8888 format data
    // Use buffer pool 2 as AI result sending buffer here
    k_u32 g_pool_id = 2;
    k_video_frame_info vf_info;
    void *pic_vaddr = NULL;
    memset(&vf_info, 0, sizeof(vf_info));
    vf_info.v_frame.width = ISP_CHN1_WIDTH;
    vf_info.v_frame.height = ISP_CHN1_HEIGHT;
    vf_info.v_frame.stride[0] = ISP_CHN1_WIDTH;
    vf_info.v_frame.pixel_format = PIXEL_FORMAT_ARGB_8888;
    k_vb_blk_handle block_enc = init_venc_frame(vf_info, &pic_vaddr, g_pool_id);
    k_u64 time_pts = 0;
    //**********************************************************************************************

    printf("start loop\n");

    uint8_t *rgb_buffer = (uint8_t *) malloc(ISP_CHN1_WIDTH * ISP_CHN1_HEIGHT * 3);

    while (!isp_stop) {
        ScopedTiming st("----------------Total time--------------- " + std::to_string(time_pts), 1);

        {
            ScopedTiming st("read capture", debug_mode);
            // Read one frame from vivcap to dump_info
            memset(&dump_info, 0, sizeof(k_video_frame_info));
            ret = kd_mpi_vicap_dump_frame(vicap_dev, VICAP_CHN_ID_1, VICAP_DUMP_YUV, &dump_info, 1000);
            if (ret) {
                printf("sample_vicap...kd_mpi_vicap_dump_frame failed. Error: %d\n", ret);
                continue;
            }
            printf("Pixel format: %d, size: %d %d\n", dump_info.v_frame.pixel_format, dump_info.v_frame.width,
                   dump_info.v_frame.height);
        }
        auto vbvaddr = kd_mpi_sys_mmap(dump_info.v_frame.phys_addr[0], size);

        std::vector<DetectionNormalized> results;

        cv::Mat rgb_frame = Utils::nv12ToRGBHWC((uint8_t *) vbvaddr,ISP_CHN1_WIDTH, ISP_CHN1_HEIGHT, rgb_buffer);

        // Copy to encoder because the rgb_frame may resized on next step
        {
            ScopedTiming st("RGB to ARGB", debug_mode);

            // Convert RGB image to ARGB image, send to encoder
            uint8_t *src = rgb_frame.data;
            uint8_t *dst = (uint8_t *) pic_vaddr;

            for (int y = 0; y < ISP_CHN1_HEIGHT; y++) {
                for (int x = 0; x < ISP_CHN1_WIDTH; x++) {
                    int src_idx = (y * ISP_CHN1_WIDTH + x) * 3;
                    int dst_idx = (y * ISP_CHN1_WIDTH + x) * 4;

                    // Copy RGB values and add alpha channel (255 = fully opaque)
                    dst[dst_idx + 0] = 255; // Alpha
                    dst[dst_idx + 1] = src[src_idx + 2]; // B
                    dst[dst_idx + 2] = src[src_idx + 1]; // G
                    dst[dst_idx + 3] = src[src_idx + 0]; // R
                }
            }
        }

        {
            ScopedTiming st("SAHI detection", 1);
            results = detect(sahi, rgb_frame, detection_max_width);
        }

        {
            ScopedTiming st("osd draw", debug_mode);

            for (int i = 0; i < results.size(); ++i) {
                const auto &det = results[i];
                auto d = Detection::from_normalized(det, dump_info.v_frame.width, dump_info.v_frame.height);
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

            // Channel 1 is decoder, channel 0 is encoder, send to channel 0, vf_info is frame data pointer, -1 means blocking mode
            vf_info.v_frame.pts = time_pts++;

            {
                last_detection_t ld;
                ld.pts = vf_info.v_frame.pts;

                for (auto it = results.cbegin(); it != results.cend(); ++it) {
                    auto d = Detection::from_normalized(*it, dump_info.v_frame.width, dump_info.v_frame.height);

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

            printf("send kd_mpi_venc_send_frame\n");
            ret = kd_mpi_venc_send_frame(0, &vf_info, -1);
            CHECK_RET(ret, __func__, __LINE__);
        }

        kd_mpi_sys_munmap(vbvaddr, size);
        ret = kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
        if (ret) {
            printf("sample_vicap...kd_mpi_vicap_dump_release failed.\n");
        }
        //break;
    }

    vivcap_stop();

    // After decoding ends, encoding ends accordingly, must release corresponding k_vb_blk_handle
    ret = kd_mpi_vb_release_block(block_enc);
    CHECK_RET(ret, __func__, __LINE__);
}

static void ipcmsg_recv(k_s32 s32Id, k_ipcmsg_message_t *msg) {
    printf("ipcmsg_recv %lu\n", msg->u32CMD);
    switch (msg->u32CMD) {
        case MSG_CMD_GET_PHY_ADDRESS: {
            auto pResp = kd_ipcmsg_create_resp_message(msg, K_SUCCESS, datafifo_phy_addr, sizeof(datafifo_phy_addr));
            kd_ipcmsg_send_only(s32Id, pResp);
            kd_ipcmsg_destroy_message(pResp);
        } break;
        default:
            break;
    }
}

void print_usage(const char *name) {
    cout << "Usage: " << name << "<kmodel> <obj_thresh> <nms_thresh> <debug_mode> " << endl
            << "For example: " << endl
            << " [for isp] ./pose_detect.elf yolov8n-pose.kmodel 0.5 0.45 0" << endl
            << "Options:" << endl
            << " 1> kmodel    Pose detection kmodel file path \n"
            << " 2> obj_thresh  Pose detection threshold\n"
            << " 3> nms_thresh  NMS threshold\n"
            << " 4> debug_mode      Debug mode: 0=no debug, 1=simple debug, 2=detailed debug\n"
            << "\n"
            << endl;
}

int main(int argc, char *argv[]) {
    {
        MediaInputConfig config {
            .sensor_width = 1920,
            .sensor_height = 1080,
            .bitrate_kbps = 4000
        };
        Media media(config);
        media.init();
        printf("-----------------------------------media init ok\n");

        uint8_t *rgb_buffer = (uint8_t *) malloc(ISP_CHN1_WIDTH * ISP_CHN1_HEIGHT * 3);

        for (int i = 0; i<10; i++) {
            auto dump = media.isp_dump();
            if (!dump) {
                printf("can't dump\n");
                break;
            }
            else {
                printf("dump\n");
                cv::Mat rgb_frame = Utils::nv12ToRGBHWC((uint8_t *) dump.value()->vbvaddr(),config.sensor_width, config.sensor_height, rgb_buffer);
                cv::imwrite("rgb.jpg", rgb_frame);

            }

        }
    }
    return 0;


    std::cout << "case " << argv[0] << " built at " << __DATE__ << " " << __TIME__ << std::endl;
    if (argc != 7) {
        print_usage(argv[0]);
        return -1;
    }

    int debug_mode = atoi(argv[1]);
    char *fd_kmodel_path = argv[2];
    float facedet_obj_thresh = atof(argv[3]);
    float facedet_nms_thresh = atof(argv[4]);
    float overlap_ratio = atof(argv[5]);
    int detection_max_width = atoi(argv[6]);

    // datafifo
    k_s32 ret = datafifo_init();
    if (0 != ret) {
        std::cout << "====== datafifo init failed ======";
    }

    k_s32 ipcmsg_handle;
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
        kd_ipcmsg_run(ipcmsg_handle);
    });


    //**********************encoder****************************************
    // Encoder configuration, encoding channel number is 0
    int chnum = 1;
    int venc_ch = 0;
    k_u32 output_frames = 10;
    k_u32 bitrate = 2000; //kbps
    int width = ISP_CHN1_WIDTH;
    int height = ISP_CHN1_HEIGHT;
    k_venc_rc_mode rc_mode = K_VENC_RC_MODE_VBR;
    k_payload_type ve_type = K_PT_H265;
    k_venc_profile profile = VENC_PROFILE_H265_MAIN;

    // VB initialization, (venc and vicap)
    ret = vb_init(chnum);
    CHECK_RET(ret, __func__, __LINE__);

    // Configure encoding channel attributes
    {
        k_venc_chn_attr ve_attr;
        memset(&ve_attr, 0, sizeof(ve_attr));
        ve_attr.venc_attr.pic_width = width;
        ve_attr.venc_attr.pic_height = height;
        ve_attr.venc_attr.stream_buf_size = VE_STREAM_BUF_SIZE;
        ve_attr.venc_attr.stream_buf_cnt = VE_OUTPUT_BUF_CNT;
        ve_attr.rc_attr.rc_mode = rc_mode;
        ve_attr.rc_attr.vbr.src_frame_rate = 10;
        ve_attr.rc_attr.vbr.dst_frame_rate = 10;
        ve_attr.rc_attr.vbr.bit_rate = bitrate;
        ve_attr.rc_attr.vbr.max_bit_rate = bitrate * 2;
        ve_attr.venc_attr.type = ve_type;
        ve_attr.venc_attr.profile = profile;
        venc_debug("payload type is H265\n");
        // Create encoding channel
        ret = kd_mpi_venc_create_chn(venc_ch, &ve_attr);
        CHECK_RET(ret, __func__, __LINE__);
    }

    // Keyframe
    kd_mpi_venc_enable_idr(venc_ch, K_TRUE);
    // Start encoding channel
    ret = kd_mpi_venc_start_chn(venc_ch);
    CHECK_RET(ret, __func__, __LINE__);

    std::thread isp_ai_detector_thread(isp_ai_detector, debug_mode, fd_kmodel_path, facedet_obj_thresh,
                                       facedet_nms_thresh, overlap_ratio, detection_max_width);

    std::thread venc_output_thread(venc_output, venc_ch);

    // example of fifo reader
    std::thread fifo_reader_thread([] {
        while (!isp_stop) {
            k_u32 readLen = 0;
            k_s32 s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_GET_AVAIL_READ_LEN, &readLen);
            if (K_SUCCESS != s32Ret)
            {
                printf("fifo_reader_thread get available read len error:%x\n", s32Ret);
                break;
            }

            if (readLen > 0)
            {
                k_char* pBuf;
                s32Ret = kd_datafifo_read(hDataFifo[READER_INDEX], (void**)&pBuf);
                if (K_SUCCESS != s32Ret)
                {
                    printf("fifo_reader_thread read error:%x\n", s32Ret);
                    break;
                }
                printf("fifo_reader_thread receive:%s\n", pBuf);
                s32Ret = kd_datafifo_cmd(hDataFifo[READER_INDEX], DATAFIFO_CMD_READ_DONE, pBuf);
                if (K_SUCCESS != s32Ret)
                {
                    printf("fifo_reader_thread read done error:%x\n", s32Ret);
                    break;
                }
            }
        }
    });

    while (getchar() != 'q') {
        usleep(10000);
    }

    isp_stop = true;
    isp_ai_detector_thread.join();

    fifo_reader_thread.join();

    venc_output_thread.join();
    kd_mpi_venc_stop_chn(venc_ch);
    kd_mpi_venc_destroy_chn(venc_ch);
    ret = kd_mpi_venc_close_fd();
    CHECK_RET(ret, __func__, __LINE__);
    free(datafifo_buf);

    kd_ipcmsg_disconnect(ipcmsg_handle);
    kd_ipcmsg_del_service(IPCMSG_NAME);
    ipcmsg_thread.join();

    // datafifo exit
    datafifo_deinit();
    // VB exit
    sample_vb_exit();

    vdec_debug("sample decode done!\n");


    return 0;
}
