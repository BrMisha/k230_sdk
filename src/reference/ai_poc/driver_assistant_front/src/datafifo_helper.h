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
namespace backend_comm { class MqttClient; }

class DatafifoHelper {
public:
    struct PendingDetections {
        std::vector<driver_assistant_detector::DetectionNormalizedCommon> detections;
        std::vector<driver_assistant_detector::DetectionNormalizedCommon> pre_detections;
        uint64_t pts;
        driver_assistant_detector::DetectedSituation situation;
        std::mutex mutex;
    };

    class Writer
    {
        k_datafifo_handle writer_handle{};
    public:
        Writer(k_datafifo_handle writer_handle) : writer_handle(writer_handle) {}

        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        Writer(Writer&&) = default;
        Writer& operator=(Writer&&) = default;

        bool write(void* pData);
    };

    DatafifoHelper(uint64_t reader_phy_addr, uint64_t writer_phy_addr, std::optional<std::string> bb_dir_path,
                   PendingDetections* pending, asio::ip::udp::socket* udp_socket,
                   websocket_server::WebSocketServer* ws_server = nullptr,
                   backend_comm::MqttClient* mqtt_client = nullptr);
    ~DatafifoHelper();

    DatafifoHelper(const DatafifoHelper&) = delete;
    DatafifoHelper& operator=(const DatafifoHelper&) = delete;

    void stop();

    std::optional<Writer> writer();

    const std::optional<std::string> bb_dir_path;

private:
    std::atomic<bool> send_stop = false;
    std::thread read_fifo_thread;

    k_datafifo_handle reader_handle;
    k_datafifo_handle writer_handle;
    PendingDetections* pending;
    asio::ip::udp::socket* udp_socket;
    websocket_server::WebSocketServer* ws_server;
    backend_comm::MqttClient* mqtt_client_;

    void read_fifo_task();
};