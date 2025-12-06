#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <atomic>
#include <vector>
#include <mutex>
#include <asio.hpp>

extern "C" {
#include "k_datafifo.h"
}

#include "../../driver_assistant_detector/common_ipc.h"

namespace websocket_server { class WebSocketServer; }

struct PendingDetections {
    std::vector<driver_assistant_detector::DetectionNormalizedCommon> detections;
    std::vector<driver_assistant_detector::DetectionNormalizedCommon> pre_detections;
    uint64_t pts;
    driver_assistant_detector::DetectedSituation situation;
    std::mutex mutex;
};

class DatafifoHelper {
public:
    DatafifoHelper(uint64_t reader_phy_addr, uint64_t writer_phy_addr, std::optional<std::string> bb_dir_path);
    ~DatafifoHelper();

    DatafifoHelper(const DatafifoHelper&) = delete;
    DatafifoHelper& operator=(const DatafifoHelper&) = delete;

    void read_fifo_task(asio::ip::udp::socket *udp_socket, std::atomic<bool>* send_stop, PendingDetections* pending, websocket_server::WebSocketServer *ws_server = nullptr);

    k_datafifo_handle reader_handle;
    k_datafifo_handle writer_handle;
    const std::optional<std::string> bb_dir_path;
};