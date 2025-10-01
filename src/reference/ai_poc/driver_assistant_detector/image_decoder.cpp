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

#define MAX_WIDTH 2560
#define MAX_HEIGHT 1920
#define STREAM_BUF_SIZE MAX_WIDTH*MAX_HEIGHT
#define FRAME_BUF_SIZE MAX_WIDTH*MAX_HEIGHT*2
#define INPUT_BUF_CNT   4
#define OUTPUT_BUF_CNT  6
#define CHANNEL_POOL_INPUT   0
#define CHANNEL_POOL_OUTPUT   1

image_decoder::image_decoder() {
    init_vb();

    k_vdec_chn_attr attr;
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
    handle = kd_mpi_vb_get_block(CHANNEL_POOL_INPUT, STREAM_BUF_SIZE, NULL);

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
