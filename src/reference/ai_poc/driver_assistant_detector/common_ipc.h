#pragma once
#include "common.h"

namespace driver_assistant_detector {
    typedef enum
    {
        MSG_CMD_GET_PHY_ADDRESS=11,
        MSG_CMD_DETECT_RGB,
        MSG_CMD_LED_SET,
        MSG_CMD_DETECTIONS,
    } ipc_msg_cmd_t;

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

}