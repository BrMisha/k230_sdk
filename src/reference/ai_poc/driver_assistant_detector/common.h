//
// Created by misha on 26/08/2025.
//

#ifndef DETECTORMODULE_COMMON_H
#define DETECTORMODULE_COMMON_H
#include <cstdint>

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
    "traffic_light_yellow"
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

    LAST
};

struct DetectionNormalizedCommon {
    detect_classes_t class_id;
    float confidence{0.0};
    float x, y, w, h;
} __attribute__((packed));
static_assert(sizeof(DetectionNormalizedCommon) == 24);

typedef enum
{
    MSG_CMD_GET_PHY_ADDRESS=11,
    MSG_CMD_DETECT_RGB,
    MSG_CMD_LED_SET,
    MSG_CMD_DETECTIONS,
} ipc_msg_cmd_t;

struct MSG_CMD_DETECT_RGB_struct {
    uint16_t width, height;
} __attribute__((packed));
static_assert(sizeof(MSG_CMD_DETECT_RGB_struct) == 4);

static const char *IPCMSG_NAME = "driver_assistant";

static const size_t DATAFIFO_DETECTOR_BLOCK_LEN = 1024*500;
static const size_t DATAFIFO_FRONT_BLOCK_LEN = 1024*1024*20;

struct DataFifoFrame_t {
    uint64_t pts;
    uint32_t data_len;
    uint8_t type;
    uint8_t reserved[3];
    uint8_t data[];
} __attribute__((packed));
static_assert(sizeof(DataFifoFrame_t) == 16);

#endif //DETECTORMODULE_COMMON_H