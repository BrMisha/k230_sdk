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
    int bitrate_kbps = 4000;
};

class MediaIspDump {
    friend class Media;

    void *_vbvaddr;
    size_t _size;

    k_video_frame_info _dump_info;
    k_vicap_dev _vicap_dev;
    k_vicap_chn _vicap_chn;

    MediaIspDump(void *vbvaddr, size_t size, k_video_frame_info dump_info, k_vicap_dev vicap_dev, k_vicap_chn vicap_chn)
    : _vbvaddr(vbvaddr), _size(size), _dump_info(dump_info), _vicap_dev(vicap_dev), _vicap_chn(vicap_chn) {}

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

    const k_u32 _venc_ch = 0;

    const k_vicap_dev _vicap_dev = VICAP_DEV_ID_0;
    const k_vicap_chn _vicap_chn_yuv420 = VICAP_CHN_ID_0;
    const k_vicap_chn _vicap_chn_rgb888 = VICAP_CHN_ID_1;

    static const k_u32 _pool_id_yuv420 = 3;
    static const k_u32 _pool_id_rgb = 4;
    static const k_u32 _pool_id_venc = 2;

    k_video_frame_info _venc_vf_info;
    void    *_venc_pic_vaddr = nullptr;
    k_vb_blk_handle _block_enc = 0;

public:
    Media(MediaInputConfig config);
    ~Media();

    k_s32 init();

    std::optional<std::unique_ptr<MediaIspDump>> isp_dump();

    k_u32 venc_get_channel() const {return _venc_ch;}
    void *venc_get_pic_vaddr() const {return _venc_pic_vaddr;}
    k_s32 venc_push(k_u64 time_pts);

private:
    k_s32 init_vb();

    k_s32 init_encoder();

    k_s32 vivcap_start();
    k_s32 vivcap_stop();

    k_vb_blk_handle init_venc_frame(k_video_frame_info &vf_info, void **pic_vaddr);

};


#endif //K230_SDK_MEDIA_H