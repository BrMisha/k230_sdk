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
    if (_phy_addr)
        kd_mpi_sys_mmz_flush_cache(_phy_addr, _vbvaddr, _size);

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
    // Unbind streaming encoder (channel 1)
    kd_mpi_sys_unbind(&_vi_mpp_chn, &_venc_stream_mpp_chn);

    // Unbind SD card encoder (channel 0)
    kd_mpi_sys_unbind(&_vi_mpp_chn, &_venc_mpp_chn);

    auto ret = vivcap_stop();
    if (ret)
        printf("Media. vivcap_stop failed ret:%d\n", ret);

    // Stop and destroy streaming encoder (channel 1)
    kd_mpi_venc_stop_chn(_venc_ch_stream);
    kd_mpi_venc_destroy_chn(_venc_ch_stream);

    // Stop and destroy SD card encoder (channel 0)
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

    k_s32 ret;

    while (1) {
        ret = vivcap_init();
        if (ret) {
            printf("ERROR vivcap_init %lu\n", ret);
            vivcap_stop();
        } else {
            // Bind YUV camera channel to SD card encoder (channel 0)
            {
                // Source: VI (camera) YUV420 channel
                _vi_mpp_chn.mod_id = K_ID_VI;
                _vi_mpp_chn.dev_id = _vicap_dev;
                _vi_mpp_chn.chn_id = _vicap_chn_yuv420;

                // Destination: VENC SD card encoder channel
                _venc_mpp_chn.mod_id = K_ID_VENC;
                _venc_mpp_chn.dev_id = 0;
                _venc_mpp_chn.chn_id = _venc_ch;

                // Unbind first in case previous run didn't clean up (e.g., Ctrl+C)
                kd_mpi_sys_unbind(&_vi_mpp_chn, &_venc_mpp_chn);

                ret = kd_mpi_sys_bind(&_vi_mpp_chn, &_venc_mpp_chn);
                if (ret)
                {
                    printf("kd_mpi_sys_bind SD failed:0x%x\n", ret);
                }
            }

            // Bind YUV camera channel to streaming encoder (channel 1)
            {
                _venc_stream_mpp_chn.mod_id = K_ID_VENC;
                _venc_stream_mpp_chn.dev_id = 0;
                _venc_stream_mpp_chn.chn_id = _venc_ch_stream;

                // Unbind first in case previous run didn't clean up
                kd_mpi_sys_unbind(&_vi_mpp_chn, &_venc_stream_mpp_chn);

                ret = kd_mpi_sys_bind(&_vi_mpp_chn, &_venc_stream_mpp_chn);
                if (ret)
                {
                    printf("kd_mpi_sys_bind stream failed:0x%x\n", ret);
                }
            }


            ret = vivcap_start();
            if (ret) {
                printf("ERROR vivcap_init %lu\n", ret);
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
    }

    return ret;
}

std::optional<std::unique_ptr<MediaIspDump>> Media::isp_dump_small_rgb888(k_video_frame_info &dump_info, k_u32 timeout_ms) {
    k_vicap_chn vicap_chn = _vicap_chn_rgb888;

    memset(&dump_info, 0, sizeof(k_video_frame_info));
    auto ret = kd_mpi_vicap_dump_frame(_vicap_dev, vicap_chn, VICAP_DUMP_RGB, &dump_info, timeout_ms);
    if (ret) {
        if (ret == K_ERR_VICAP_BUF_EMPTY)
            printf("sample_vicap...kd_mpi_vicap_dump_frame failed. Error: %d\n", ret);
        return std::nullopt;
    }

    size_t size = (dump_info.v_frame.width * dump_info.v_frame.height * 3);
    auto vbvaddr = kd_mpi_sys_mmap_cached(dump_info.v_frame.phys_addr[0], size);

    if (vbvaddr == nullptr) {
        kd_mpi_vicap_dump_release(_vicap_dev, vicap_chn, &dump_info);
        return std::nullopt;
    }

    auto d = new MediaIspDump(dump_info.v_frame.phys_addr[0], vbvaddr, size, std::move(dump_info), _vicap_dev, vicap_chn);

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

    auto d = new MediaIspDump(0, vbvaddr, size, std::move(dump_info), _vicap_dev, _vicap_chn_yuv420);

    return std::unique_ptr<MediaIspDump>(d);
}

k_s32 Media::init_vb() {
    k_s32 ret = 0;
    k_vb_config vb_config;
    memset(&vb_config, 0, sizeof(vb_config));

    vb_config.max_pool_cnt = 5;  // 4 pools used (0-3)

    k_u64 pic_size = _input_config.sensor_width * _input_config.sensor_height * 2;
    k_u64 stream_size = _input_config.sensor_width * _input_config.sensor_height / 2;

    // Pool 0: VICAP YUV420 input
    vb_config.comm_pool[0].blk_cnt = 6;
    vb_config.comm_pool[0].blk_size = VICAP_ALIGN_UP(pic_size, 0x1000);
    vb_config.comm_pool[0].mode = VB_REMAP_MODE_NOCACHE;

    // Pool 1: SD card encoder output (channel 0)
    vb_config.comm_pool[1].blk_cnt = 30;
    vb_config.comm_pool[1].blk_size = VICAP_ALIGN_UP(stream_size, 0x1000);
    vb_config.comm_pool[1].mode = VB_REMAP_MODE_NOCACHE;

    // Pool 2: Streaming encoder output (channel 1)
    vb_config.comm_pool[2].blk_cnt = 15;
    vb_config.comm_pool[2].blk_size = VICAP_ALIGN_UP(stream_size, 0x1000);
    vb_config.comm_pool[2].mode = VB_REMAP_MODE_NOCACHE;

    // Pool 3: RGB888 for AI processing
    static_assert(_pool_id_rgb888 == 3);
    vb_config.comm_pool[_pool_id_rgb888].blk_cnt = 3;
    vb_config.comm_pool[_pool_id_rgb888].mode = VB_REMAP_MODE_CACHED;
    vb_config.comm_pool[_pool_id_rgb888].blk_size = VICAP_ALIGN_UP(_input_config.rgb888_width * _input_config.rgb888_height * 3, 0x1000);

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
    k_venc_rc_mode rc_mode = K_VENC_RC_MODE_CBR;  // CBR for predictable SD card file sizes
    k_payload_type ve_type = K_PT_H265;
    k_venc_profile profile = VENC_PROFILE_H265_MAIN;

    k_s32 ret = 0;
    k_u64 stream_size = _input_config.sensor_width * _input_config.sensor_height / 2;

    // Configure encoding channel attributes for SD card recording
    {
        k_venc_chn_attr ve_attr;
        memset(&ve_attr, 0, sizeof(ve_attr));
        ve_attr.venc_attr.pic_width = _input_config.sensor_width;
        ve_attr.venc_attr.pic_height = _input_config.sensor_height;
        ve_attr.venc_attr.stream_buf_size = stream_size;
        ve_attr.venc_attr.stream_buf_cnt = 15;
        ve_attr.rc_attr.rc_mode = rc_mode;
        ve_attr.rc_attr.cbr.gop = 60;  // Keyframe every 2 seconds at 30fps
        ve_attr.rc_attr.cbr.src_frame_rate = 30;
        ve_attr.rc_attr.cbr.dst_frame_rate = 30;
        ve_attr.rc_attr.cbr.bit_rate = _input_config.bitrate_kbps;
        ve_attr.venc_attr.type = ve_type;
        ve_attr.venc_attr.profile = profile;

        // Create encoding channel
        ret = kd_mpi_venc_create_chn(_venc_ch, &ve_attr);
        if (ret) {
            printf("Media. kd_mpi_venc_create_chn failed ret:%d\n", ret);
            return ret;
        }
    }

    // Enable IDR frame request for SD card encoder
    ret = kd_mpi_venc_enable_idr(_venc_ch, K_TRUE);
    if (ret) {
        printf("Media. kd_mpi_venc_enable_idr failed ret:%d\n", ret);
        return ret;
    }
    // Start SD card encoding channel
    ret = kd_mpi_venc_start_chn(_venc_ch);
    if (ret) {
        printf("Media. kd_mpi_venc_start_chn failed ret:%d\n", ret);
        return ret;
    }

    // Configure streaming encoder (channel 1) with VBR for adaptive streaming
    {
        k_venc_chn_attr ve_attr;
        memset(&ve_attr, 0, sizeof(ve_attr));
        ve_attr.venc_attr.pic_width = _input_config.sensor_width;
        ve_attr.venc_attr.pic_height = _input_config.sensor_height;
        ve_attr.venc_attr.stream_buf_size = stream_size;
        ve_attr.venc_attr.stream_buf_cnt = 15;
        ve_attr.rc_attr.rc_mode = K_VENC_RC_MODE_VBR;
        ve_attr.rc_attr.vbr.gop = 30*2;  // Keyframe every 1 second at 30fps
        ve_attr.rc_attr.vbr.src_frame_rate = 30;
        ve_attr.rc_attr.vbr.dst_frame_rate = 30;
        ve_attr.rc_attr.vbr.bit_rate = _input_config.stream_bitrate_kbps;
        ve_attr.rc_attr.vbr.max_bit_rate = _input_config.stream_bitrate_kbps * 2;
        ve_attr.venc_attr.type = ve_type;
        ve_attr.venc_attr.profile = profile;

        ret = kd_mpi_venc_create_chn(_venc_ch_stream, &ve_attr);
        if (ret) {
            printf("Media. kd_mpi_venc_create_chn stream failed ret:%d\n", ret);
            return ret;
        }
    }

    // Enable IDR frame request for streaming encoder
    ret = kd_mpi_venc_enable_idr(_venc_ch_stream, K_TRUE);
    if (ret) {
        printf("Media. kd_mpi_venc_enable_idr stream failed ret:%d\n", ret);
        return ret;
    }
    // Start streaming encoding channel
    ret = kd_mpi_venc_start_chn(_venc_ch_stream);
    if (ret)
        printf("Media. kd_mpi_venc_start_chn stream failed ret:%d\n", ret);

    return ret;
}

k_s32 Media::vivcap_init()
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
    if (_input_config.rotate_camera) {
        dev_attr.mirror = VICAP_MIRROR_BOTH;  // 180 degree rotation
    }
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
    chn_attr.buffer_num = 10;
    chn_attr.alignment = 12;  // 4096-byte (page) alignment for DMA (2^12 = 4096)
    // chn_attr.buffer_size = config.comm_pool[0].blk_size;
    chn_attr.buffer_size = VICAP_ALIGN_UP((_input_config.sensor_width * _input_config.sensor_height * 3) / 2, 0x1000);

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
    chn_attr.buffer_size = VICAP_ALIGN_UP((_input_config.sensor_height * _input_config.sensor_width * 3 ), 0x1000);

    /*printf("sample_vicap ...kd_mpi_vicap_set_chn_attr rgb, buffer_size[%d]\n", chn_attr.buffer_size);
    ret = kd_mpi_vicap_set_chn_attr(_vicap_dev, _vicap_chn_rgb888, chn_attr);
    if (ret) {
        printf("Media. kd_mpi_vicap_set_chn_attr failed.\n");
        return ret;
    }*/

    //set chn2 output rgb888p
    chn_attr.out_win.width = _input_config.rgb888_width;
    chn_attr.out_win.height = _input_config.rgb888_height;
    chn_attr.buffer_size = VICAP_ALIGN_UP((_input_config.rgb888_height * _input_config.rgb888_width * 3 ), 0x1000);
    chn_attr.buffer_num = 30;
    ret = kd_mpi_vicap_set_chn_attr(_vicap_dev, _vicap_chn_rgb888, chn_attr);
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

    return ret;
}

k_s32 Media::vivcap_start() {
    auto ret = kd_mpi_vicap_start_stream(_vicap_dev);
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