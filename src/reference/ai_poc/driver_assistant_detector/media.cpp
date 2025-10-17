//
// Created by misha on 29/09/2025.
//

#include "media.h"
#include "mpi_vb_api.h"
#include "mpi_venc_api.h"
#include "mpi_vicap_api.h"
#include "mpi_sys_api.h"
#include <cstdio>
#include <thread>
#include <unistd.h>

MediaIspDump::~MediaIspDump() {
    kd_mpi_sys_munmap(_vbvaddr, _size);
    auto ret = kd_mpi_vicap_dump_release(_vicap_dev, _vicap_chn, &_dump_info);
    if (ret) {
        printf("MediaIspDump::~MediaIspDump. kd_mpi_vicap_dump_release failed.\n");
    }
}

Media::Media(MediaInputConfig config) :
_input_config(config)
{

}

Media::~Media() {
    auto ret = vivcap_stop();
    if (ret)
        printf("Media. vivcap_stop failed ret:%d\n", ret);

    kd_mpi_vb_release_block(_block_enc);
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
    memset(&_venc_vf_info, 0, sizeof(k_video_frame_info));
    _venc_vf_info.v_frame.width = _input_config.sensor_width;
    _venc_vf_info.v_frame.height = _input_config.sensor_height;
    _venc_vf_info.v_frame.stride[0] = _input_config.sensor_width;
    _venc_vf_info.v_frame.pixel_format = PIXEL_FORMAT_ARGB_8888;
    _block_enc = init_venc_frame(_venc_vf_info, &_venc_pic_vaddr);

    k_s32 ret;

    while (1) {
        ret = vivcap_start();
        if (ret) {
            printf("ERROR vivcap_start %lu\n", ret);
            vivcap_stop();
        } else {
            sleep(2);
            k_video_frame_info dump_info;
            memset(&dump_info, 0, sizeof(k_video_frame_info));
            ret = kd_mpi_vicap_dump_frame(_vicap_dev, _vicap_chn_rgb888, VICAP_DUMP_YUV, &dump_info, 1000);
            if (ret) {
                printf("ERROR kd_mpi_vicap_dump_frame %lu\n", ret);

                ret = kd_mpi_vicap_dump_release(_vicap_dev, _vicap_chn_rgb888, &dump_info);
                if (ret) {
                    printf("ERROR kd_mpi_vicap_dump_release %lu\n", ret);
                }

                vivcap_stop();
            } else {
                kd_mpi_vicap_dump_release(_vicap_dev, _vicap_chn_rgb888, &dump_info);
                break;
            }
        }
    }

    return ret;
}

std::optional<std::unique_ptr<MediaIspDump>> Media::isp_dump_rgb888(k_video_frame_info &dump_info, uint8_t channel) {
    k_vicap_chn vicap_chn = channel == 0 ? _vicap_chn_rgb888 : _vicap_chn_rgb888_2;

    memset(&dump_info, 0, sizeof(k_video_frame_info));
    //kd_mpi_vicap_3d_mode_crtl(K_FALSE);
    auto ret = kd_mpi_vicap_dump_frame(_vicap_dev, vicap_chn, VICAP_DUMP_RGB, &dump_info, 1000);
    if (ret) {
        printf("sample_vicap...kd_mpi_vicap_dump_frame failed. Error: %d\n", ret);
        return std::nullopt;
    }
    //kd_mpi_vicap_3d_mode_crtl(K_TRUE);
    size_t size = (dump_info.v_frame.width * dump_info.v_frame.height * 3);
    auto vbvaddr = kd_mpi_sys_mmap(dump_info.v_frame.phys_addr[0], size);

    if (vbvaddr == nullptr) {
        kd_mpi_vicap_dump_release(_vicap_dev, vicap_chn, &dump_info);
        return std::nullopt;
    }

    auto d = new MediaIspDump(vbvaddr, size, std::move(dump_info), _vicap_dev, vicap_chn);

    return std::unique_ptr<MediaIspDump>(d);
}

std::optional<std::unique_ptr<MediaIspDump>> Media::isp_dump_yuv420(k_video_frame_info &dump_info) {
    memset(&dump_info, 0, sizeof(k_video_frame_info));
    auto ret = kd_mpi_vicap_dump_frame(_vicap_dev, _vicap_chn_yuv420, VICAP_DUMP_YUV, &dump_info, 1000);
    if (ret) {
        printf("sample_vicap...kd_mpi_vicap_dump_frame failed. Error: %d\n", ret);
        return std::nullopt;
    }

    size_t size = (dump_info.v_frame.width * dump_info.v_frame.height * 3) / 2;
    auto vbvaddr = kd_mpi_sys_mmap(dump_info.v_frame.phys_addr[0], size);

    if (vbvaddr == nullptr) {
        kd_mpi_vicap_dump_release(_vicap_dev, _vicap_chn_yuv420, &dump_info);
        return std::nullopt;
    }

    auto d = new MediaIspDump(vbvaddr, size, std::move(dump_info), _vicap_dev, _vicap_chn_yuv420);

    return std::unique_ptr<MediaIspDump>(d);
}

k_s32 Media::venc_push(k_u64 time_pts) {
    _venc_vf_info.v_frame.pts = time_pts;
    return kd_mpi_venc_send_frame(0, &_venc_vf_info, -1);
}

k_s32 Media::init_vb() {
    k_s32 ret = 0;
    k_vb_config vb_config;
    memset(&vb_config, 0, sizeof(vb_config));

    vb_config.max_pool_cnt = 6;

    k_u64 pic_size = _input_config.sensor_width * _input_config.sensor_height * 2;
    k_u64 stream_size = _input_config.sensor_width * _input_config.sensor_height / 2;
    vb_config.comm_pool[0].blk_cnt = 6;
    vb_config.comm_pool[0].blk_size = ((pic_size + 0xfff) & ~0xfff);
    vb_config.comm_pool[0].mode = VB_REMAP_MODE_NOCACHE;
    vb_config.comm_pool[1].blk_cnt = 30;
    vb_config.comm_pool[1].blk_size = ((stream_size + 0xfff) & ~0xfff);
    vb_config.comm_pool[1].mode = VB_REMAP_MODE_NOCACHE;
    static_assert(_pool_id_venc == 2);
    vb_config.comm_pool[_pool_id_venc].blk_cnt = 4;
    vb_config.comm_pool[_pool_id_venc].blk_size = (_input_config.sensor_width * _input_config.sensor_height * 4);
    vb_config.comm_pool[_pool_id_venc].mode = VB_REMAP_MODE_NOCACHE;

    //VB for YUV420SP output
    static_assert(_pool_id_yuv420 == 3);
    vb_config.comm_pool[_pool_id_yuv420].blk_cnt = 3;
    vb_config.comm_pool[_pool_id_yuv420].mode = VB_REMAP_MODE_NOCACHE;
    vb_config.comm_pool[_pool_id_yuv420].blk_size = VICAP_ALIGN_UP((_input_config.sensor_width * _input_config.sensor_height * 3) / 2,
        VICAP_ALIGN_1K);


    //VB for RGB888 output
    static_assert(_pool_id_rgb == 4);
    vb_config.comm_pool[_pool_id_rgb].blk_cnt = 3;
    vb_config.comm_pool[_pool_id_rgb].mode = VB_REMAP_MODE_NOCACHE;
    vb_config.comm_pool[_pool_id_rgb].blk_size = VICAP_ALIGN_UP(_input_config.sensor_width * _input_config.sensor_height * 3, VICAP_ALIGN_1K);

    //VB for RGB888_2 output
    static_assert(_pool_id_rgb_2 == 5);
    vb_config.comm_pool[_pool_id_rgb_2].blk_cnt = 3;
    vb_config.comm_pool[_pool_id_rgb_2].mode = VB_REMAP_MODE_NOCACHE;
    vb_config.comm_pool[_pool_id_rgb_2].blk_size = VICAP_ALIGN_UP(_input_config.rgb888_2_width * _input_config.rgb888_2_height * 3, VICAP_ALIGN_1K);

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

    // Optimized for traffic light detection: maximize contrast and detail
    dev_attr.pipe_ctrl.data = 0x00000000;  // Start with all disabled

    // ESSENTIAL - Basic image formation
    dev_attr.pipe_ctrl.bits.demosaic_enable = 1;  // Convert Bayer → RGB
    dev_attr.pipe_ctrl.bits.ccm_enable = 1;       // Color correction for accurate R/G/Y
    dev_attr.pipe_ctrl.bits.wb_enable = 1;        // White balance for color accuracy
    dev_attr.pipe_ctrl.bits.dpcc_enable = 1;      // Remove dead pixels

    // CRITICAL FOR CONTRAST & DETAIL
    dev_attr.pipe_ctrl.bits.cproc_enable = 1;     // High contrast/saturation
    dev_attr.pipe_ctrl.bits.ee_enable = 1;        // Edge enhancement (sharpening)
    dev_attr.pipe_ctrl.bits.gc_enable = 1;        // Gamma for better contrast

    // WIDE DYNAMIC RANGE - handles bright lights without saturation
    dev_attr.pipe_ctrl.bits.wdr_enable = 1;       // Hardware WDR (no latency)

    // OPTIONAL - Lens correction
    dev_attr.pipe_ctrl.bits.lsc_enable = 1;       // Fix vignetting

    // ENABLE Auto Exposure for adaptive brightness
    dev_attr.pipe_ctrl.bits.ae_enable = 1;        // AUTO exposure - adapts to lighting
    dev_attr.pipe_ctrl.bits.awb_enable = 1;       // AUTO white balance
    dev_attr.pipe_ctrl.bits.af_enable = 0;        // No autofocus needed
    dev_attr.pipe_ctrl.bits.ahdr_enable = 0;      // No multi-frame HDR (use WDR instead)

    // DISABLE - Noise reduction (blurs edges, reduces detail)
    dev_attr.pipe_ctrl.bits.cnr_enable = 0;       // Color NR blurs
    dev_attr.pipe_ctrl.bits.ynr_enable = 0;       // Luma NR blurs
    dev_attr.pipe_ctrl.bits.dnr2_enable = 0;      // 2D denoise blurs
    dev_attr.pipe_ctrl.bits.dnr3_enable = 0;      // 3D denoise blurs
    dev_attr.pipe_ctrl.bits.dpf_enable = 0;       // Prefilter blurs

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
    chn_attr.buffer_num = 3;  // Minimum buffers for real-time (reduce latency)
    // chn_attr.buffer_size = config.comm_pool[0].blk_size;
    chn_attr.buffer_size = VICAP_ALIGN_UP((_input_config.sensor_width * _input_config.sensor_height * 3) / 2, VICAP_ALIGN_1K);

    printf("sample_vicap ...kd_mpi_vicap_set_chn_attr yuv, buffer_size[%d]\n", chn_attr.buffer_size);
    ret = kd_mpi_vicap_set_chn_attr(_vicap_dev, _vicap_chn_yuv420, chn_attr);
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
    chn_attr.pix_format = PIXEL_FORMAT_RGB_888;
    chn_attr.buffer_num = 3;  // Minimum buffers for real-time (reduce latency)
    // chn_attr.buffer_size = config.comm_pool[1].blk_size;
    chn_attr.buffer_size = VICAP_ALIGN_UP((_input_config.sensor_height * _input_config.sensor_width * 3 ), VICAP_ALIGN_1K);

    printf("sample_vicap ...kd_mpi_vicap_set_chn_attr rgb, buffer_size[%d]\n", chn_attr.buffer_size);
    ret = kd_mpi_vicap_set_chn_attr(_vicap_dev, _vicap_chn_rgb888, chn_attr);
    if (ret) {
        printf("Media. kd_mpi_vicap_set_chn_attr failed.\n");
        return ret;
    }

    //set chn2 output rgb888p
    chn_attr.out_win.width = _input_config.rgb888_2_width;
    chn_attr.out_win.height = _input_config.rgb888_2_height;
    chn_attr.buffer_size = VICAP_ALIGN_UP((_input_config.rgb888_2_height * _input_config.rgb888_2_width * 3 ), VICAP_ALIGN_1K);
    ret = kd_mpi_vicap_set_chn_attr(_vicap_dev, _vicap_chn_rgb888_2, chn_attr);
    if (ret) {
        printf("Media. kd_mpi_vicap_set_chn_attr failed.\n");
        return ret;
    }

    // set to header file database parse mode
    /*ret = kd_mpi_vicap_set_database_parse_mode(vicap_dev, VICAP_DATABASE_PARSE_XML_JSON);
    if (ret) {
        printf("sample_vicap, kd_mpi_vicap_set_database_parse_mode failed.\n");
        return ret;
    }*/

    //printf("sample_vicap ...kd_mpi_vicap_init\n");
    ret = kd_mpi_vicap_init(_vicap_dev);
    if (ret) {
        printf("Media. kd_mpi_vicap_init failed.\n");
        // goto err_exit;
    }

    // AE (Auto Exposure) is enabled - it will automatically adjust exposure and gain

    //printf("sample_vicap ...kd_mpi_vicap_start_stream\n");
    ret = kd_mpi_vicap_start_stream(_vicap_dev);
    if (ret) {
        printf("Media. kd_mpi_vicap_start_stream failed.\n");
        // goto err_exit;
    }

    return ret;
}

k_s32 Media::vivcap_stop()
{
    //printf("sample_vicap ...kd_mpi_vicap_stop_stream\n");
    int ret = kd_mpi_vicap_stop_stream(_vicap_dev);
    if (ret) {
        printf("Media. kd_mpi_vicap_stop_stream failed.\n");
        return ret;
    }

    ret = kd_mpi_vicap_deinit(_vicap_dev);
    if (ret) {
        printf("Media. kd_mpi_vicap_deinit failed.\n");
    }

    return ret;
}

k_vb_blk_handle Media::init_venc_frame(k_video_frame_info &vf_info, void **pic_vaddr) {
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

    handle = kd_mpi_vb_get_block(_pool_id_venc, size, NULL);
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
    vf_info.pool_id = _pool_id_venc;
    vf_info.v_frame.phys_addr[0] = phys_addr;
    if (vf_info.v_frame.pixel_format == PIXEL_FORMAT_YVU_PLANAR_420)
        vf_info.v_frame.phys_addr[1] = phys_addr + (vf_info.v_frame.height * vf_info.v_frame.stride[0]);
    *pic_vaddr = virt_addr;

    printf("phys_addr is %lx g_pool_id is %d \n", phys_addr, _pool_id_venc);

    return handle;
}