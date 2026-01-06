//
// Created by misha on 29/09/2025.
//

#ifndef K230_SDK_MEDIA_H
#define K230_SDK_MEDIA_H

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <optional>

#include "k_vicap_comm.h"
#include "k_venc_comm.h"
#include "k_connector_comm.h"
#include "k_type.h"
#include "k_vb_comm.h"

struct MediaInputConfig {
    int sensor_width = 1920;
    int sensor_height = 1080;
    int rgb888_width = 1920;
    int rgb888_height = 1080;
    int bitrate_kbps = 12000;         // 12 Mbps for good 1080p30 SD card quality
    int stream_bitrate_kbps = 4000;   // 4 Mbps for streaming
    bool rotate_camera = false;
};

class MediaIspDump {
    friend class Media;

    k_u64 _phy_addr;
    void *_vbvaddr;
    size_t _size;

    k_video_frame_info _dump_info;
    k_vicap_dev _vicap_dev;
    k_vicap_chn _vicap_chn;

    MediaIspDump(k_u64 phy_addr, void *vbvaddr, size_t size, k_video_frame_info dump_info, k_vicap_dev vicap_dev, k_vicap_chn vicap_chn)
    : _phy_addr(phy_addr), _vbvaddr(vbvaddr), _size(size), _dump_info(dump_info), _vicap_dev(vicap_dev), _vicap_chn(vicap_chn) {}

    // Prevent copying, allow moving
    MediaIspDump(const MediaIspDump&) = delete;
    MediaIspDump& operator=(const MediaIspDump&) = delete;
    MediaIspDump(MediaIspDump&&) = default;
    MediaIspDump& operator=(MediaIspDump&&) = default;

public:
    ~MediaIspDump();

    [[nodiscard]] void * vbvaddr() const {
        return _vbvaddr;
    }

    [[nodiscard]] size_t size() const {
        return _size;
    }

};

class Media {
    MediaInputConfig _input_config;

    // SD card encoder (channel 0)
    const k_u32 _venc_ch = 0;
    k_mpp_chn _venc_mpp_chn;
    k_mpp_chn _vi_mpp_chn;

    // Streaming encoder (channel 1)
    const k_u32 _venc_ch_stream = 1;
    k_mpp_chn _venc_stream_mpp_chn;

    const k_vicap_dev _vicap_dev = VICAP_DEV_ID_0;
    const k_vicap_chn _vicap_chn_rgb888 = VICAP_CHN_ID_0;
    const k_vicap_chn _vicap_chn_yuv420 = VICAP_CHN_ID_1;

    static const k_u32 _pool_id_rgb888 = 3;  // Pool 3 for RGB888 (after encoder pools)

public:
    Media(MediaInputConfig config);
    ~Media();

    k_s32 init();

    MediaInputConfig const *input_config() const { return &_input_config;}

    std::optional<std::unique_ptr<MediaIspDump>> isp_dump_small_rgb888(k_video_frame_info &dump_info, k_u32 timeout_ms = 1000);
    std::optional<std::unique_ptr<MediaIspDump>> isp_dump_yuv420(k_video_frame_info &dump_info);

    k_u32 venc_get_channel() const { return _venc_ch; }
    k_u32 venc_get_stream_channel() const { return _venc_ch_stream; }

private:
    k_s32 init_vb();

    k_s32 init_encoder();

    k_s32 vivcap_init();
    k_s32 vivcap_start();
    k_s32 vivcap_stop();

};


#endif //K230_SDK_MEDIA_H