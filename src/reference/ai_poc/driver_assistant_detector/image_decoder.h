//
// Created by misha on 01/10/2025.
//

#ifndef K230_SDK_IMAGE_DECODER_H
#define K230_SDK_IMAGE_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <optional>

#include "k_vicap_comm.h"
#include "k_venc_comm.h"
#include "k_connector_comm.h"
#include "k_type.h"
#include "k_vb_comm.h"

class image_decoder {
    public:
    image_decoder();
    ~image_decoder();

    void push_data(const uint8_t* data, size_t data_size);
    bool get_frame();

private:
    k_s32 init_vb();
};


#endif //K230_SDK_IMAGE_DECODER_H