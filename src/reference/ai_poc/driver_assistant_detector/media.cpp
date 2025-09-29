//
// Created by misha on 29/09/2025.
//

#include "media.h"
#include "mpi_vb_api.h"
#include "mpi_venc_api.h"
#include "mpi_vicap_api.h"
#include <cstdio>

Media::Media(MediaInputConfig config) :
_input_config(config)
{

}

Media::~Media() {
    auto ret = vivcap_stop();
    if (ret)
        printf("Media. vivcap_stop failed ret:%d\n", ret);

    kd_mpi_venc_stop_chn(_venc_ch);
    kd_mpi_venc_destroy_chn(_venc_ch);
    ret = kd_mpi_venc_close_fd();
    if (ret)
        printf("Media. kd_mpi_venc_close_fd failed ret:%d\n", ret);

    ret = kd_mpi_vb_exit();
    if (ret)
        printf("Media. kd_mpi_vb_exit failed ret:%d\n", ret);
}

k_s32 Media::init() {
    init_vb();
    init_encoder();
    return vivcap_start();
}

k_s32 Media::init_vb() {
    k_s32 ret = 0;
    k_vb_config vb_config;
    memset(&vb_config, 0, sizeof(vb_config));

    vb_config.max_pool_cnt = 4;

    k_u64 pic_size = _input_config.sensor_width * _input_config.sensor_height * 2;
    k_u64 stream_size = _input_config.sensor_width * _input_config.sensor_height / 2;
    vb_config.comm_pool[0].blk_cnt = 6;
    vb_config.comm_pool[0].blk_size = ((pic_size + 0xfff) & ~0xfff);
    vb_config.comm_pool[0].mode = VB_REMAP_MODE_NOCACHE;
    vb_config.comm_pool[1].blk_cnt = 30;
    vb_config.comm_pool[1].blk_size = ((stream_size + 0xfff) & ~0xfff);
    vb_config.comm_pool[1].mode = VB_REMAP_MODE_NOCACHE;

    //VB for YUV420SP output
    vb_config.comm_pool[2].blk_cnt = 6;
    vb_config.comm_pool[2].mode = VB_REMAP_MODE_NOCACHE;
    vb_config.comm_pool[2].blk_size = VICAP_ALIGN_UP((_input_config.sensor_width * _input_config.sensor_height * 3) / 2,
        VICAP_ALIGN_1K);

    //VB for RGB888 output
    vb_config.comm_pool[3].blk_cnt = 5;
    vb_config.comm_pool[3].mode = VB_REMAP_MODE_NOCACHE;
    vb_config.comm_pool[3].blk_size = VICAP_ALIGN_UP(_input_config.sensor_width * _input_config.sensor_height * 3, VICAP_ALIGN_1K);

    ret = kd_mpi_vb_set_config(&vb_config);
    if (ret) {
        printf("Media. kd_mpi_vb_set_config failed ret:%d\n", ret);
        return ret;
    }

    k_vb_supplement_config supplement_config;
    memset(&supplement_config, 0, sizeof(supplement_config));
    supplement_config.supplement_config |= VB_SUPPLEMENT_JPEG_MASK;
    ret = kd_mpi_vb_set_supplement_config(&supplement_config);
    if (ret) {
        printf("Media. vb_set_supplement_config failed ret:%d\n", ret);
        return ret;
    }

    ret = kd_mpi_vb_init();
    if (ret)
        printf("Media. vb_init failed ret:%d\n", ret);

    return ret;
}

k_s32 Media::init_encoder() {
    k_venc_rc_mode rc_mode = K_VENC_RC_MODE_VBR;
    k_payload_type ve_type = K_PT_H265;
    k_venc_profile profile = VENC_PROFILE_H265_MAIN;

    k_s32 ret = 0;
    k_u64 stream_size = _input_config.sensor_width * _input_config.sensor_height / 2;


    // Configure encoding channel attributes
    {
        k_venc_chn_attr ve_attr;
        memset(&ve_attr, 0, sizeof(ve_attr));
        ve_attr.venc_attr.pic_width = _input_config.sensor_width;
        ve_attr.venc_attr.pic_height = _input_config.sensor_height;
        ve_attr.venc_attr.stream_buf_size = stream_size;
        ve_attr.venc_attr.stream_buf_cnt = 15;
        ve_attr.rc_attr.rc_mode = rc_mode;
        ve_attr.rc_attr.vbr.src_frame_rate = 10;
        ve_attr.rc_attr.vbr.dst_frame_rate = 10;
        ve_attr.rc_attr.vbr.bit_rate = _input_config.bitrate_kbps;
        ve_attr.rc_attr.vbr.max_bit_rate = _input_config.bitrate_kbps * 2;
        ve_attr.venc_attr.type = ve_type;
        ve_attr.venc_attr.profile = profile;

        // Create encoding channel
        ret = kd_mpi_venc_create_chn(_venc_ch, &ve_attr);
        if (ret) {
            printf("Media. kd_mpi_venc_create_chn failed ret:%d\n", ret);
            return ret;
        }
    }

    // Keyframe
    ret = kd_mpi_venc_enable_idr(_venc_ch, K_TRUE);
    if (ret) {
        printf("Media. kd_mpi_venc_enable_idr failed ret:%d\n", ret);
        return ret;
    }
    // Start encoding channel
    ret = kd_mpi_venc_start_chn(_venc_ch);
    if (ret)
        printf("Media. kd_mpi_venc_start_chn failed ret:%d\n", ret);

    return ret;
}

k_s32 Media::vivcap_start()
{
    k_u32 pool_id;
    k_vb_pool_config pool_config;

    k_vicap_sensor_type sensor_type = OV_OV5647_MIPI_CSI0_1920X1080_30FPS_10BIT_LINEAR;

    k_vicap_sensor_info sensor_info;
    memset(&sensor_info, 0, sizeof(k_vicap_sensor_info));
    k_s32 ret = kd_mpi_vicap_get_sensor_info(sensor_type, &sensor_info);
    if (ret) {
        printf("sample_vicap, the sensor type not supported!\n");
        return ret;
    }

    /* Configure sensor device attributes to prepare for ISP initialization */
    k_vicap_dev_attr dev_attr;
    memset(&dev_attr, 0, sizeof(k_vicap_dev_attr));
    dev_attr.acq_win.h_start = 0; /* No horizontal offset for ISP input frame */
    dev_attr.acq_win.v_start = 0; /* No vertical offset for ISP input frame */
    dev_attr.acq_win.width = _input_config.sensor_width; /* ISP input image width */
    dev_attr.acq_win.height = _input_config.sensor_height; /* ISP input image height */
    dev_attr.mode = VICAP_WORK_ONLINE_MODE; /* Online mode, raw data from sensor does not need memory buffering */
    //dev_attr.mode = VICAP_WORK_OFFLINE_MODE;

    dev_attr.pipe_ctrl.data = 0xFFFFFFFF;
    dev_attr.pipe_ctrl.bits.af_enable = 0; /* No AF function */
    dev_attr.pipe_ctrl.bits.ahdr_enable = 0; /* Not HDR */
    dev_attr.dw_enable = K_FALSE;

    dev_attr.cpature_frame = 0; /* Continuously capture images */
    memcpy(&dev_attr.sensor_info, &sensor_info, sizeof(k_vicap_sensor_info));

    /* Configure VICAP device attributes */
    ret = kd_mpi_vicap_set_dev_attr(_vicap_dev, dev_attr);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_set_dev_attr failed.\n");
        return ret;
    }

    k_vicap_chn_attr chn_attr;
    memset(&chn_attr, 0, sizeof(k_vicap_chn_attr));

    //set chn0 output yuv420sp
    chn_attr.out_win.h_start = 0;
    chn_attr.out_win.v_start = 0;

    chn_attr.out_win.width = _input_config.sensor_width;
    chn_attr.out_win.height = _input_config.sensor_height;

    chn_attr.crop_win = dev_attr.acq_win;
    chn_attr.scale_win = chn_attr.out_win;
    chn_attr.crop_enable = K_FALSE;
    chn_attr.scale_enable = K_FALSE;
    chn_attr.chn_enable = K_TRUE;
    chn_attr.pix_format = PIXEL_FORMAT_YVU_PLANAR_420;
    chn_attr.buffer_num = 5 - 1;//at least 3 buffers for isp
    // chn_attr.buffer_size = config.comm_pool[0].blk_size;
    chn_attr.buffer_size = VICAP_ALIGN_UP((_input_config.sensor_width * _input_config.sensor_height * 3) / 2, VICAP_ALIGN_1K);

    printf("sample_vicap ...kd_mpi_vicap_set_chn_attr, buffer_size[%d]\n", chn_attr.buffer_size);
    ret = kd_mpi_vicap_set_chn_attr(_vicap_dev, _vicap_chn, chn_attr);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_set_chn_attr failed.\n");
        return ret;
    }

    //set chn1 output rgb888p
    chn_attr.out_win.h_start = 0;
    chn_attr.out_win.v_start = 0;
    chn_attr.out_win.width = _input_config.sensor_width;
    chn_attr.out_win.height = _input_config.sensor_height;

    chn_attr.crop_win = dev_attr.acq_win;
    chn_attr.scale_win = chn_attr.out_win;
    chn_attr.crop_enable = K_FALSE;
    chn_attr.scale_enable = K_FALSE;
    chn_attr.chn_enable = K_TRUE;
    chn_attr.pix_format = PIXEL_FORMAT_BGR_888_PLANAR;
    chn_attr.buffer_num = 5;//at least 3 buffers for isp
    // chn_attr.buffer_size = config.comm_pool[1].blk_size;
    chn_attr.buffer_size = VICAP_ALIGN_UP((_input_config.sensor_height * _input_config.sensor_width * 3 ), VICAP_ALIGN_1K);

    printf("sample_vicap ...kd_mpi_vicap_set_chn_attr, buffer_size[%d]\n", chn_attr.buffer_size);
    ret = kd_mpi_vicap_set_chn_attr(_vicap_dev, VICAP_CHN_ID_1, chn_attr);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_set_chn_attr failed.\n");
        return ret;
    }
    // set to header file database parse mode
    /*ret = kd_mpi_vicap_set_database_parse_mode(vicap_dev, VICAP_DATABASE_PARSE_XML_JSON);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_set_database_parse_mode failed.\n");
        return ret;
    }*/

    printf("sample_vicap ...kd_mpi_vicap_init\n");
    ret = kd_mpi_vicap_init(_vicap_dev);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_init failed.\n");
        // goto err_exit;
    }

    printf("sample_vicap ...kd_mpi_vicap_start_stream\n");
    ret = kd_mpi_vicap_start_stream(_vicap_dev);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_init failed.\n");
        // goto err_exit;
    }

    return ret;
}

k_s32 Media::vivcap_stop()
{
    printf("sample_vicap ...kd_mpi_vicap_stop_stream\n");
    int ret = kd_mpi_vicap_stop_stream(_vicap_dev);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_init failed.\n");
        return ret;
    }

    ret = kd_mpi_vicap_deinit(_vicap_dev);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_deinit failed.\n");
    }

    return ret;
}