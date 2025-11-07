//
// Created by misha on 26/08/2025.
//

#ifndef DETECTORMODULE_COMMON_H
#define DETECTORMODULE_COMMON_H
#include <cstdint>

namespace driver_assistant_detector {
    const std::vector<std::string> detect_classes_str{
        "objects-5YaV",
        "arrow_right",
        "color_green",
        "color_red",
        "tl_arrow_forward",
        "tl_arrow_left",
        "traffic_light",
        "traffic_light_green",
        "traffic_light_red",
        "traffic_light_red_yellow",
        "traffic_light_yellow",

        "",  // LAST placeholder

        "approved_green",
        "approved_red",
        "approved_yellow",
        "approved_red_yellow"
    };

    enum detect_classes_t {
        OBJECTS_5YaV = 0,
        ARROW_RIGHT,
        COLOR_GREEN,
        COLOR_RED,
        TL_ARROW_FORWARD,
        TL_ARROW_LEFT,
        TRAFFIC_LIGHT,
        TRAFFIC_LIGHT_GREEN,
        TRAFFIC_LIGHT_RED,
        TRAFFIC_LIGHT_RED_YELLOW,
        TRAFFIC_LIGHT_YELLOW,

        LAST,

        APPROVED_GREEN,
        APPROVED_RED,
        APPROVED_YELLOW,
        APPROVED_RED_YELLOW,
    };

    constexpr int DETECTOR_CLASSES_SIZE = detect_classes_t::LAST;

    struct DetectionNormalizedCommon {
        detect_classes_t class_id;
        float confidence{0.0};
        float x, y, w, h;
    } __attribute__((packed));
    static_assert(sizeof(DetectionNormalizedCommon) == 24);

    struct MSG_CMD_DETECT_RGB_struct {
        uint16_t width, height;
    } __attribute__((packed));
    static_assert(sizeof(MSG_CMD_DETECT_RGB_struct) == 4);
}

#endif //DETECTORMODULE_COMMON_H