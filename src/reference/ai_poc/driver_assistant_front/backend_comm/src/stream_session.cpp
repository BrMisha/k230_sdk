#include "stream_session.h"
#include <iostream>
#include <nlohmann/json.hpp>

StreamSession::StreamSession(std::shared_ptr<backend_comm::MqttClient> mqtt,
                             std::string user_id,
                             std::string session_id,
                             std::vector<IceServer> ice_servers)
    : user_id(std::move(user_id))
    , session_id(std::move(session_id))
    , mqtt_(std::move(mqtt))
    , ice_servers_(std::move(ice_servers))
{
}

StreamSession::~StreamSession()
{
    if (active_.load()) {
        stop();
    }
}

bool StreamSession::start()
{
    if (active_.load()) {
        return true;  // Already started
    }

    std::cout << "[StreamSession] Starting - user: " << user_id
              << ", session: " << session_id << std::endl;

    // Subscribe to all messages from client
    std::string topic = topic_from_client();
    if (!mqtt_->subscribe(topic, 1)) {
        std::cerr << "[StreamSession] Failed to subscribe to " << topic << std::endl;
        return false;
    }

    active_.store(true);
    std::cout << "[StreamSession] Started, waiting for 'watch' message" << std::endl;
    return true;
}

void StreamSession::stop()
{
    if (!active_.load()) {
        return;
    }

    std::cout << "[StreamSession] Stopping - user: " << user_id
              << ", session: " << session_id << std::endl;

    // Clean up WebRTC peer first
    webrtc_peer_.reset();

    mqtt_->unsubscribe(topic_from_client());

    active_.store(false);
    std::cout << "[StreamSession] Stopped" << std::endl;
}

void StreamSession::on_message(const std::vector<std::string>& path_parts, const std::string& payload)
{
    if (!active_.load()) {
        std::cout << "[StreamSession] Ignoring message - session not active" << std::endl;
        return;
    }

    if (path_parts.empty()) {
        std::cout << "[StreamSession] Empty path" << std::endl;
        return;
    }

    const std::string& category = path_parts[0];
    const std::string type = path_parts.size() > 1 ? path_parts[1] : "";

    std::cout << "[StreamSession] Received message - category: " << category << ", type: " << type << std::endl;

    if (category == "signaling") {
        // Handle signaling messages
        if (type == "watch") {
            handle_watch(payload);
        } else if (type == "answer") {
            handle_answer(payload);
        } else if (type == "ice") {
            handle_ice(payload);
        } else if (type == "stop") {
            handle_stop(payload);
        } else {
            std::cout << "[StreamSession] Unknown signaling type: " << type << std::endl;
        }
    } else if (category == "ping") {
        // Respond with pong
        send_message("pong", payload);
    } else {
        // Future: handle other categories (commands, state, etc.)
        std::cout << "[StreamSession] Unknown category: " << category << std::endl;
    }
}

void StreamSession::handle_watch(const std::string& payload)
{
    std::cout << "[StreamSession] Received 'watch' - app wants to start streaming" << std::endl;

    // Create WebRTC peer if not already created
    if (!webrtc_peer_) {
        webrtc_peer_ = std::make_unique<GstWebRTCPeer>(ice_servers_);

        // Use weak_ptr to safely capture session lifetime in callbacks
        // This prevents crashes if callbacks fire after session destruction
        std::weak_ptr<StreamSession> weak_self = shared_from_this();

        // Set up callbacks with weak_ptr safety
        webrtc_peer_->set_on_local_description([weak_self](const std::string& type, const std::string& sdp) {
            if (auto self = weak_self.lock()) {
                nlohmann::json j = {{"type", type}, {"sdp", sdp}};
                self->send_signaling_message("offer", j.dump());
            }
        });

        webrtc_peer_->set_on_ice_candidate([weak_self](guint mlineindex, const std::string& candidate) {
            if (auto self = weak_self.lock()) {
                nlohmann::json j = {{"mlineindex", mlineindex}, {"candidate", candidate}};
                self->send_signaling_message("ice", j.dump());
            }
        });

        webrtc_peer_->set_on_state_change([weak_self](bool connected) {
            if (auto self = weak_self.lock()) {
                std::cout << "[StreamSession] WebRTC " << (connected ? "connected" : "disconnected") << std::endl;
            }
        });

        webrtc_peer_->set_on_data_channel_message([weak_self](const std::string& message) {
            if (auto self = weak_self.lock()) {
                std::cout << "[StreamSession] Data channel message: " << message << std::endl;
                // Echo back for testing
                if (self->webrtc_peer_) {
                    self->webrtc_peer_->send_data("echo: " + message);
                }
            }
        });
    }

    // Create offer (this triggers negotiation)
    webrtc_peer_->create_offer();
}

void StreamSession::handle_answer(const std::string& payload)
{
    std::cout << "[StreamSession] Received 'answer' - app sent SDP answer" << std::endl;

    if (!webrtc_peer_) {
        std::cerr << "[StreamSession] No WebRTC peer - ignoring answer" << std::endl;
        return;
    }

    try {
        auto j = nlohmann::json::parse(payload);
        std::string type = j.value("type", "answer");
        std::string sdp = j.value("sdp", "");

        if (sdp.empty()) {
            std::cerr << "[StreamSession] Empty SDP in answer" << std::endl;
            return;
        }

        webrtc_peer_->set_remote_description(type, sdp);
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[StreamSession] Failed to parse answer: " << e.what() << std::endl;
    }
}

void StreamSession::handle_ice(const std::string& payload)
{
    std::cout << "[StreamSession] Received 'ice' - ICE candidate from app" << std::endl;
    std::cout << "[StreamSession] ICE JSON: " << payload << std::endl;

    if (!webrtc_peer_) {
        std::cerr << "[StreamSession] No WebRTC peer - ignoring ICE candidate" << std::endl;
        return;
    }

    try {
        auto j = nlohmann::json::parse(payload);
        guint mlineindex = j.value("sdpMLineIndex", 0u);
        std::cout << "[StreamSession] payload " << payload << std::endl;
        std::string candidate = j.value("candidate", "");

        if (candidate.empty()) {
            std::cout << "[StreamSession] Empty ICE candidate (end of candidates)" << std::endl;
            return;
        }

        webrtc_peer_->add_ice_candidate(mlineindex, candidate);
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[StreamSession] Failed to parse ICE candidate: " << e.what() << std::endl;
    }
}

void StreamSession::handle_stop(const std::string& payload)
{
    std::cout << "[StreamSession] Received 'stop' - app wants to stop streaming" << std::endl;
    stop();
}

bool StreamSession::send_message(const std::string& type, const std::string& payload)
{
    if (!active_.load()) {
        std::cerr << "[StreamSession] Cannot send message - session not active" << std::endl;
        return false;
    }

    std::cout << "[StreamSession] Sending message - type: " << type << std::endl;
    return mqtt_->publish(topic_from_device(type), payload, 1, false);
}

bool StreamSession::send_signaling_message(const std::string& type, const std::string& payload)
{
    if (!active_.load()) {
        std::cerr << "[StreamSession] Cannot send signaling - session not active" << std::endl;
        return false;
    }

    std::string topic = topic_from_device("signaling/" + type);
    std::cout << "[StreamSession] Sending signaling: " << type << std::endl;

    return mqtt_->publish(topic, payload, 1, false);
}

std::string StreamSession::topic_from_client() const
{
    // v1/sessions/{serial}/{user_id}/{session_id}/from-client/#
    return "v1/sessions/" + mqtt_->get_serial() + "/" + user_id + "/" + session_id + "/from-client/#";
}

std::string StreamSession::topic_from_device(const std::string& path) const
{
    // v1/sessions/{serial}/{user_id}/{session_id}/from-device/{path}
    return "v1/sessions/" + mqtt_->get_serial() + "/" + user_id + "/" + session_id + "/from-device/" + path;
}

void StreamSession::push_video_frame(const uint8_t* data, size_t size, uint64_t pts_us, bool is_keyframe)
{
    if (!webrtc_peer_ || !webrtc_peer_->is_connected()) {
        return;
    }

    // h265parse needs VPS/SPS/PPS before it can parse P-frames
    // Wait for first keyframe (which includes header) before pushing any frames
    if (waiting_for_keyframe_.load()) {
        if (!is_keyframe) {
            // Skip P-frames until we get a keyframe
            return;
        }
        // Got keyframe - stop waiting
        waiting_for_keyframe_.store(false);
        std::cout << "[StreamSession] First keyframe received, starting video push" << std::endl;
    }

    webrtc_peer_->push_video_frame(data, size, pts_us, is_keyframe);
}