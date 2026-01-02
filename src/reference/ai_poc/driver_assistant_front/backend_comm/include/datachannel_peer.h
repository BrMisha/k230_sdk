#pragma once

#include <rtc/rtc.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <cstdint>

namespace backend_comm {

class DataChannelPeer {
public:
    // Callback types (same as GstWebRTCPeer for compatibility)
    using OnLocalDescription = std::function<void(const std::string& type, const std::string& sdp)>;
    using OnIceCandidate = std::function<void(uint32_t mlineindex, const std::string& candidate)>;
    using OnStateChange = std::function<void(bool connected)>;
    using OnDataChannelMessage = std::function<void(const std::string& message)>;

    explicit DataChannelPeer(const std::vector<std::string>& ice_servers);
    ~DataChannelPeer();

    // Non-copyable, non-movable
    DataChannelPeer(const DataChannelPeer&) = delete;
    DataChannelPeer& operator=(const DataChannelPeer&) = delete;
    DataChannelPeer(DataChannelPeer&&) = delete;
    DataChannelPeer& operator=(DataChannelPeer&&) = delete;

    // WebRTC signaling
    void create_offer();
    void set_remote_description(const std::string& type, const std::string& sdp);
    void add_ice_candidate(uint32_t mlineindex, const std::string& candidate);

    // Data channel operations
    void send_video_frame(const uint8_t* data, size_t size, uint64_t pts_us, uint8_t type);
    void send_data(const std::string& message);
    void send_binary(const uint8_t* data, size_t size);

    // State query
    bool is_connected() const { return connected_.load(); }

    // Callback setters
    void set_on_local_description(OnLocalDescription cb);
    void set_on_ice_candidate(OnIceCandidate cb);
    void set_on_state_change(OnStateChange cb);
    void set_on_data_channel_message(OnDataChannelMessage cb);

private:
    void setup_callbacks();
    void setup_data_channel(std::shared_ptr<rtc::DataChannel> dc);

    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::DataChannel> dc_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> offer_sent_{false};

    // Callbacks
    OnLocalDescription on_local_description_;
    OnIceCandidate on_ice_candidate_;
    OnStateChange on_state_change_;
    OnDataChannelMessage on_data_channel_message_;
    std::mutex callback_mutex_;

    // Frame chunking for WebRTC data channel (same protocol as before)
    uint32_t frame_count_ = 0;
    uint64_t pframe_count_ = 0;
    static constexpr size_t MAX_CHUNK_SIZE = 15 * 1024;  // 15KB chunks (under 16KB SCTP limit)
};

}  // namespace backend_comm
