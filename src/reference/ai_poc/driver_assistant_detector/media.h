//
// Created by misha on 29/09/2025.
//

#ifndef K230_SDK_MEDIA_H
#define K230_SDK_MEDIA_H

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include "k_vicap_comm.h"
#include "k_venc_comm.h"
#include "k_connector_comm.h"
#include "k_type.h"

struct MediaInputConfig {
    int sensor_width = 1920;
    int sensor_height = 1080;
    int bitrate_kbps = 4000;
};

class Media {
    MediaInputConfig _input_config;

    int _venc_ch = 0;

    k_vicap_dev _vicap_dev = VICAP_DEV_ID_0;
    k_vicap_chn _vicap_chn = VICAP_CHN_ID_0;

public:
    Media(MediaInputConfig config);
    ~Media();

    k_s32 init();


private:
    k_s32 init_vb();

    k_s32 init_encoder();

    k_s32 vivcap_start();
    k_s32 vivcap_stop();

};


#endif //K230_SDK_MEDIA_H