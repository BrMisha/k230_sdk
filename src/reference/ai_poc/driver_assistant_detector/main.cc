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


#include <iostream>
#include <thread>
#include <chrono>
#include <memory>
#include "utils.h"
#include "vi_vo.h"
#include "ob_det.h"
#include "sahi.h"

using std::cerr;
using std::cout;
using std::endl;

std::atomic<bool> isp_stop(false);

void print_usage(const char *name)
{
    cout << "Usage: " << name << "<kmodel_det> <score_thres> <nms_thres> <input_mode> <debug_mode> <overlap_ratio>" << endl
         << "Options:" << endl
         << "  kmodel_det      多目标检测 kmodel路径\n"
         << "  score_thres     多目标检测 分数阈值\n"
         << "  nms_thres       多目标检测 非极大值抑制阈值\n"
         << "  input_mode      本地图片(图片路径)/ 摄像头(None) \n"
         << "  debug_mode      是否需要调试，0、1、2分别表示不调试、简单调试、详细调试\n"
         << "  overlap_ratio   SAHI重叠比率 (0.0-0.5, 例如: 0.2)\n"
         << "\n"
         << endl;
}

void video_proc_sahi(char *argv[])
{
    vivcap_start();

    k_video_frame_info vf_info;
    void *pic_vaddr = NULL;       //osd

    memset(&vf_info, 0, sizeof(vf_info));

    vf_info.v_frame.width = osd_width;
    vf_info.v_frame.height = osd_height;
    vf_info.v_frame.stride[0] = osd_width;
    vf_info.v_frame.pixel_format = PIXEL_FORMAT_ARGB_8888;
    block = vo_insert_frame(&vf_info, &pic_vaddr);

    // alloc memory
    size_t paddr = 0;
    void *vaddr = nullptr;
    size_t size = SENSOR_CHANNEL * SENSOR_HEIGHT * SENSOR_WIDTH;
    int ret = kd_mpi_sys_mmz_alloc_cached(&paddr, &vaddr, "allocate", "anonymous", size);
    if (ret)
    {
        std::cerr << "physical_memory_block::allocate failed: ret = " << ret << ", errno = " << strerror(errno) << std::endl;
        std::abort();
    }

    const int MAX_SIZE = atoi(argv[7]);

    // Create standard OBDet and SAHI (without physical memory addresses)
    OBDet obDet(argv[1], atof(argv[2]), atof(argv[3]), atoi(argv[5]));
    SAHI sahi(&obDet, cv::Size(320, 320), atof(argv[6]));

    std::vector<Detection> results;

    while (!isp_stop)
    {
        ScopedTiming st("total time", 1);

        {
            ScopedTiming st("read capture", 1); //0.006741 ms
            // VICAP_CHN_ID_1 out rgb888p
            memset(&dump_info, 0 , sizeof(k_video_frame_info));
            ret = kd_mpi_vicap_dump_frame(vicap_dev, VICAP_CHN_ID_1, VICAP_DUMP_YUV, &dump_info, 1000);
            if (ret) {
                printf("sample_vicap...kd_mpi_vicap_dump_frame failed.\n");
                continue;
            }
        }
            

        {
            ScopedTiming st("isp copy", 1); //1.65852 ms
            // 从vivcap中读取一帧图像到dump_info
            auto vbvaddr = kd_mpi_sys_mmap_cached(dump_info.v_frame.phys_addr[0], size);
            memcpy(vaddr, (void *)vbvaddr, SENSOR_HEIGHT * SENSOR_WIDTH * 3);  // 这里以后可以去掉，不用copy
            kd_mpi_sys_munmap(vbvaddr, size);
        }

        // Convert planar RGB buffer to cv::Mat
        int matsize = SENSOR_WIDTH * SENSOR_HEIGHT;
        cv::Mat ori_img;
        {
            ScopedTiming st("convert to cv::Mat", 1); //1.60641 ms
            cv::Mat ori_img_R = cv::Mat(SENSOR_HEIGHT, SENSOR_WIDTH, CV_8UC1, vaddr);
            cv::Mat ori_img_G = cv::Mat(SENSOR_HEIGHT, SENSOR_WIDTH, CV_8UC1, vaddr + 1 * matsize);
            cv::Mat ori_img_B = cv::Mat(SENSOR_HEIGHT, SENSOR_WIDTH, CV_8UC1, vaddr + 2 * matsize);
            std::vector<cv::Mat> sensor_rgb;
            sensor_rgb.push_back(ori_img_R);
            sensor_rgb.push_back(ori_img_G);
            sensor_rgb.push_back(ori_img_B);
            cv::merge(sensor_rgb, ori_img);
        }

        // Use SAHI for detection
        {
            if (ori_img.cols > MAX_SIZE) {
                float scale = static_cast<float>(MAX_SIZE) / ori_img.cols;
                int new_width = MAX_SIZE;
                int new_height = static_cast<int>(ori_img.rows * scale);
                cv::resize(ori_img, ori_img, cv::Size(new_width, new_height));
                ori_img.cols = new_width;
                ori_img.rows = new_height;
                std::cout << "Resized image to: " << ori_img.cols << "x" << ori_img.rows << std::endl;

                cv::imwrite("scaled.jpg", ori_img);
            }

            //ScopedTiming st("SAHI detection", atoi(argv[5]));
            auto st = std::make_unique<ScopedTiming>("SAHI detection", 1);
            results.clear();
            results = sahi.detect(ori_img);
            st.reset();


            for (int i = 0; i < results.size(); ++i) {
                const auto& det = results[i];
                std::cout << "Object " << (i+1) << ": "
                          << det.className << " (ID:" << det.class_id << ") "
                          << "confidence=" << det.confidence << " "
                          << "box=[" << det.box.x << "," << det.box.y << ","
                          << det.box.width << "x" << det.box.height << "]"
                          << std::endl;
            }

            Utils::draw_detections(ori_img, results);
            cv::imwrite("object_det.jpg", ori_img);
        }

        cv::Mat osd_frame(osd_height, osd_width, CV_8UC4, cv::Scalar(0, 0, 0, 0));

        #if defined(STUDIO_HDMI)
        {
            ScopedTiming st("osd draw", atoi(argv[5]));
            Utils::draw_detections(osd_frame, results, {osd_frame.cols, osd_frame.rows}, {SENSOR_WIDTH, SENSOR_HEIGHT});
        }
        #else
        {
            ScopedTiming st("osd draw", 1);
            cv::rotate(osd_frame, osd_frame, cv::ROTATE_90_COUNTERCLOCKWISE);
            Utils::draw_detections(osd_frame, results, {osd_frame.cols, osd_frame.rows}, {SENSOR_WIDTH, SENSOR_HEIGHT});
            cv::rotate(osd_frame, osd_frame, cv::ROTATE_90_CLOCKWISE);
        }
        #endif

        {
            ScopedTiming st("osd copy", 1);
            memcpy(pic_vaddr, osd_frame.data, osd_width * osd_height * 4);
            //显示通道插入帧
            kd_mpi_vo_chn_insert_frame(osd_id+3, &vf_info);  //K_VO_OSD0

            ret = kd_mpi_vicap_dump_release(vicap_dev, VICAP_CHN_ID_1, &dump_info);
            if (ret) {
                printf("sample_vicap...kd_mpi_vicap_dump_release failed.\n");
            }
        }
    }

    vo_osd_release_block();
    vivcap_stop();


    // free memory
    ret = kd_mpi_sys_mmz_free(paddr, vaddr);
    if (ret)
    {
        std::cerr << "free failed: ret = " << ret << ", errno = " << strerror(errno) << std::endl;
        std::abort();
    }
}


int main(int argc, char *argv[])
{
    std::cout << "case " << argv[0] << " built at " << __DATE__ << " " << __TIME__ << std::endl;
    if (argc != 8)
    {
        print_usage(argv[0]);
        return -1;
    }

    if (strcmp(argv[4], "None") == 0)
    {
        // Check if SAHI mode is requested (overlap_ratio > 0)
        std::thread thread_isp;
        thread_isp = std::thread(video_proc_sahi, argv);
        while (getchar() != 'q')
        {
            usleep(10000);
        }

        isp_stop = true;
        thread_isp.join();
    }
    else
    {
        cv::Mat ori_img = cv::imread(argv[4]);
        int ori_w = ori_img.cols;
        int ori_h = ori_img.rows;

        std::cout << "Original image size: " << ori_w << "x" << ori_h << std::endl;
        
        // Resize if width is more than specified size, maintaining aspect ratio
        const int MAX_SIZE = atoi(argv[7]);
        if (ori_w > MAX_SIZE) {
            float scale = static_cast<float>(MAX_SIZE) / ori_w;
            int new_width = MAX_SIZE;
            int new_height = static_cast<int>(ori_h * scale);
            cv::resize(ori_img, ori_img, cv::Size(new_width, new_height));
            ori_w = new_width;
            ori_h = new_height;
            std::cout << "Resized image to: " << ori_w << "x" << ori_h << std::endl;

            cv::imwrite("scaled.jpg", ori_img);
        }

        OBDet obDet(argv[1], atof(argv[2]), atof(argv[3]), atoi(argv[5]));
        /*
        obDet.pre_process(ori_img);

        obDet.inference();

        std::vector<Detection> results;
        obDet.post_process({ori_w, ori_h}, results);*/

        SAHI sahi(&obDet, cv::Size(320, 320), atof(argv[6]));
        
        auto start_time = std::chrono::high_resolution_clock::now();
        std::vector<Detection> results = sahi.detect(ori_img);
        auto end_time = std::chrono::high_resolution_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        std::cout << "SAHI detection took: " << duration.count() << " ms" << std::endl;
        std::cout << "Detected " << results.size() << " objects." << std::endl;
        
        // Print detailed information about each detection
        for (int i = 0; i < results.size(); ++i) {
            const auto& det = results[i];
            std::cout << "Object " << (i+1) << ": " 
                      << det.className << " (ID:" << det.class_id << ") "
                      << "confidence=" << det.confidence << " "
                      << "box=[" << det.box.x << "," << det.box.y << "," 
                      << det.box.width << "x" << det.box.height << "]"
                      << std::endl;
        }

        Utils::draw_detections(ori_img, results);
        cv::imwrite("object_det.jpg", ori_img);
    }
    return 0;
}