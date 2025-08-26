//
// Created by misha on 26/08/2025.
//

#ifndef DETECTORMODULE_COMMON_H
#define DETECTORMODULE_COMMON_H
#include <cstdint>

struct DetectionCommon {
    char className[24];
    float confidence{0.0};
    uint16_t x, y, w, h;
} __attribute__((packed));

static_assert(sizeof(DetectionCommon) == 36, "Detection size must be 36 bytes");

#endif //DETECTORMODULE_COMMON_H