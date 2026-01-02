#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <vector>
#include "mqtt_client.h"
#include "datachannel_peer.h"

/**
 * StreamSession - Handles streaming session over MQTT (signaling + WebRTC)
 *
 * Lifecycle:
 *   1. stream_start command received with {user_id, session_id, ice_servers}
 *   2. MqttClient creates StreamSession, subscribes to signaling topic
 *   3. App sends 'watch' message
 *   4. Device creates WebRTC offer and sends via MQTT
 *   5. App sends 'answer' with SDP
 *   6. ICE candidates exchanged via MQTT
 *   7. WebRTC connection established
 *   8. stream_stop command ends session
 */
class StreamSession : public std::enable_shared_from_this<StreamSession> {
public:
    // Session identifiers (immutable after construction)
    const std::string user_id;
    const std::string session_id;

    StreamSession(std::shared_ptr<backend_comm::MqttClient> mqtt,
                  std::string user_id,
                  std::string session_id,
                  std::vector<std::string> ice_servers);
    ~StreamSession();

    // Non-copyable
    StreamSession(const StreamSession&) = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    /**
     * Start signaling session - subscribe to signaling topic
     * @return true if subscribed successfully
     */
    bool start();

    /**
     * Stop signaling session - unsubscribe from signaling topic
     */
    void stop();

    /**
     * Handle incoming message from MQTT
     * Called by MqttClient when message matches this session
     * @param path_parts Path parts after /from-client/ (e.g., {"signaling", "watch"})
     * @param payload JSON payload
     */
    void on_message(const std::vector<std::string>& path_parts, const std::string& payload);

    /**
     * Check if session is active
     */
    bool is_active() const { return active_.load(); }

    /**
     * Push video frame to WebRTC peer via data channel (if connected)
     * type: opaque byte passed through to receiver (e.g., keyframe/P-frame/header)
     */
    void push_video_frame(const uint8_t* data, size_t size, uint64_t pts_us, uint8_t type);

private:
    // Message handlers
    void handle_watch(const std::string& payload);
    void handle_answer(const std::string& payload);
    void handle_ice(const std::string& payload);
    void handle_stop(const std::string& payload);

    // Send generic message (e.g., "pong" → .../from-device/pong)
    bool send_message(const std::string& type, const std::string& payload);

    // Send signaling message (e.g., "offer" → .../from-device/signaling/offer)
    bool send_signaling_message(const std::string& type, const std::string& payload);

    // Topic construction
    std::string topic_from_client() const;  // .../from-client/#
    std::string topic_from_device(const std::string& path) const;  // .../from-device/{path}

    std::shared_ptr<backend_comm::MqttClient> mqtt_;
    std::atomic<bool> active_{false};

    // WebRTC
    std::vector<std::string> ice_servers_;
    std::unique_ptr<backend_comm::DataChannelPeer> webrtc_peer_;
};
