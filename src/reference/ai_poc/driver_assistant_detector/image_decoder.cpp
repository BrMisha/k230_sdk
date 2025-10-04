//
// Created by misha on 01/10/2025.
//

#include "image_decoder.h"
#include "mpi_vb_api.h"
#include "mpi_vdec_api.h"
#include "mpi_vicap_api.h"
#include "mpi_sys_api.h"
#include <cstdio>
#include <thread>
#include <unistd.h>
#include <cstdlib>
#include <opencv2/opencv.hpp>
#include "utils.h"

#define MAX_WIDTH 2592
#define MAX_HEIGHT 2048
#define STREAM_BUF_SIZE MAX_WIDTH*MAX_HEIGHT
#define FRAME_BUF_SIZE MAX_WIDTH*MAX_HEIGHT*4
#define INPUT_BUF_CNT   1
#define OUTPUT_BUF_CNT  1
#define CHANNEL_POOL_INPUT   0
#define CHANNEL_POOL_OUTPUT   1

static inline void CHECK_RET(k_s32 ret, const char *func, const int line)
{
    if (ret)
        printf("image_decoder. error ret %d, func %s line %d\n", ret, func, line);
}

image_decoder::image_decoder() {
    init_vb();

    k_vdec_chn_attr attr;
    attr.mode = K_VDEC_SEND_MODE_STREAM;
    attr.pic_width = MAX_WIDTH;
    attr.pic_height = MAX_HEIGHT;
    attr.frame_buf_cnt = OUTPUT_BUF_CNT;
    attr.frame_buf_size = FRAME_BUF_SIZE;
    attr.stream_buf_size = STREAM_BUF_SIZE;
    attr.type = K_PT_JPEG;
    attr.frame_buf_pool_id = CHANNEL_POOL_OUTPUT;
    auto ret = kd_mpi_vdec_create_chn(0, &attr);
    if (ret)
        printf("image_decoder. kd_mpi_vdec_create_chn failed ret:%d\n", ret);

    ret = kd_mpi_vdec_start_chn(0);
    if (ret)
        printf("image_decoder. kd_mpi_vdec_start_chn failed ret:%d\n", ret);
}

image_decoder::~image_decoder() {
    kd_mpi_vdec_stop_chn(0);
    kd_mpi_vdec_destroy_chn(0);

    auto ret = kd_mpi_vb_exit();
    if (ret)
        printf("image_decoder. kd_mpi_vb_exit failed ret:%d\n", ret);

}

void image_decoder::push_data(const uint8_t *data, size_t data_size) {
    k_vdec_stream stream;
    memset(&stream, 0, sizeof(k_vdec_stream));

    auto handle = kd_mpi_vb_get_block(CHANNEL_POOL_INPUT, STREAM_BUF_SIZE, NULL);

    if (handle == VB_INVALID_HANDLE)
    {
        printf("image_decoder. kd_mpi_vb_get_block failed VB_INVALID_HANDLE\n");
        return;
    }

    k_s32 pool_id = kd_mpi_vb_handle_to_pool_id(handle);
    if (pool_id == VB_INVALID_POOLID)
    {
        printf("image_decoder. kd_mpi_vb_handle_to_pool_id failed VB_INVALID_POOLID\n");
        return;
    }

    auto phys_addr = kd_mpi_vb_handle_to_phyaddr(handle);
    if (phys_addr == 0)
    {
        printf("%s get phys addr error\n", __func__);
        return;
    }

    printf("pool_id %d, blk_size %d\n", pool_id, STREAM_BUF_SIZE);

    auto virt_addr = (k_u8 *)kd_mpi_sys_mmap_cached(phys_addr, STREAM_BUF_SIZE);

    if (virt_addr == NULL)
    {
        printf("%s mmap error\n", __func__);
        return;
    }



    FILE *file = fopen("pic.jpg", "rb");
    if (file == NULL)
    {
        printf("%s failed to open pic.jpg\n", __func__);
        return;
    }
    size_t bytes_read = fread(virt_addr, 1, STREAM_BUF_SIZE, file);
    fclose(file);

    stream.end_of_stream = K_TRUE;

    auto ret = kd_mpi_sys_mmz_flush_cache(phys_addr, virt_addr, bytes_read);
    CHECK_RET(ret, __func__, __LINE__);
    printf("%s read %zu bytes from pic.jpg\n", __func__, bytes_read);

    stream.phy_addr = phys_addr;
    stream.len = bytes_read;

    ret = kd_mpi_vdec_send_stream(0, &stream, -1);
    CHECK_RET(ret, __func__, __LINE__);

    ret = kd_mpi_sys_munmap((void *)virt_addr, STREAM_BUF_SIZE);
    CHECK_RET(ret, __func__, __LINE__);

    ret = kd_mpi_vb_release_block(handle);
    CHECK_RET(ret, __func__, __LINE__);

    printf("rrrrrrrrrrrrrr\n");
}

bool image_decoder::get_frame() {
    k_vdec_chn_status status;
    k_vdec_supplement_info supplement;
    k_video_frame_info output;

    auto ret = kd_mpi_vdec_query_status(0, &status);
    CHECK_RET(ret, __func__, __LINE__);

    if (status.end_of_stream)
    {
        printf("%s, receive eos\n", __func__);
        //return false;
    }

    printf("status.width:%d status.height:%d type:%d\n",status.width, status.height, status.type);

    ret = kd_mpi_vdec_get_frame(0, &output, &supplement, -1);
    CHECK_RET(ret, __func__, __LINE__);
    printf("Frame valid: %d, format: %d\n", supplement.is_valid_frame, output.v_frame.pixel_format);

    auto frame_size = (status.width*status.height*3)/2;
    auto virt_addr = kd_mpi_sys_mmap_cached(output.v_frame.phys_addr[0], frame_size);
    printf("virt_addr:%p\n", virt_addr);

    FILE *dump_file = fopen("dump", "wb");
    if (dump_file != NULL)
    {
        fwrite(virt_addr, 1, frame_size, dump_file);
        fclose(dump_file);
        printf("Saved %d bytes to dump\n", frame_size);
    }

    uint8_t *rgb_buffer = (uint8_t *) malloc( status.width * status.height * 3);
    cv::Mat rgb_frame = Utils::nv12ToRGBHWC((uint8_t *) virt_addr, status.width, status.height, rgb_buffer);

    cv::imwrite("rgb_frame.jpg", rgb_frame);

    kd_mpi_sys_munmap(virt_addr, frame_size);
    kd_mpi_vdec_release_frame(0, &output);
    CHECK_RET(ret, __func__, __LINE__);

    return true;
}

k_s32 image_decoder::init_vb() {
    k_s32 ret = 0;
    k_vb_config vb_config;
    memset(&vb_config, 0, sizeof(vb_config));

    vb_config.max_pool_cnt = 2;

    vb_config.comm_pool[CHANNEL_POOL_INPUT].blk_cnt = INPUT_BUF_CNT;
    vb_config.comm_pool[CHANNEL_POOL_INPUT].blk_size = STREAM_BUF_SIZE;
    vb_config.comm_pool[CHANNEL_POOL_INPUT].mode = VB_REMAP_MODE_NOCACHE;
    vb_config.comm_pool[CHANNEL_POOL_OUTPUT].blk_cnt = OUTPUT_BUF_CNT;
    vb_config.comm_pool[CHANNEL_POOL_OUTPUT].blk_size = FRAME_BUF_SIZE;
    vb_config.comm_pool[CHANNEL_POOL_OUTPUT].mode = VB_REMAP_MODE_NOCACHE;

    ret = kd_mpi_vb_set_config(&vb_config);
    if (ret) {
        printf("image_decoder. kd_mpi_vb_set_config failed ret:%d\n", ret);
        return ret;
    }

    k_vb_supplement_config supplement_config;
    memset(&supplement_config, 0, sizeof(supplement_config));
    supplement_config.supplement_config |= VB_SUPPLEMENT_JPEG_MASK;
    ret = kd_mpi_vb_set_supplement_config(&supplement_config);
    if (ret) {
        printf("image_decoder. vb_set_supplement_config failed ret:%d\n", ret);
        return ret;
    }

    ret = kd_mpi_vb_init();
    if (ret)
        printf("image_decoder. vb_init failed ret:%d\n", ret);

    return ret;
}
