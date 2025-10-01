//
// Created by misha on 26/08/2025.
//

#ifndef DETECTORMODULE_COMMON_H
#define DETECTORMODULE_COMMON_H
#include <cstdint>

struct DetectionCommon {
    int class_id;
    float confidence{0.0};
    uint16_t x, y, w, h;
} __attribute__((packed));

static_assert(sizeof(DetectionCommon) == 16, "Detection size must be 36 bytes");

const std::vector<std::string> detect_classes{"objects-5YaV", "arrow_right", "tl_arrow_forward", "tl_arrow_left", "tl_green", "tl_red",
          "traffic_light", "traffic_light_back", "traffic_light_green", "traffic_light_red", "traffic_light_yellow"};

typedef enum
{
    MSG_CMD_GET_PHY_ADDRESS=11,
} ipc_msg_cmd_t;

inline const char *IPCMSG_NAME = "driver_assistant";

static const size_t DATAFIFO_BLOCK_LEN = 1024000;

#endif //DETECTORMODULE_COMMON_H