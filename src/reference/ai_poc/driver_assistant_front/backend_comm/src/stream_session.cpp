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

    // Stop publish thread
    if (publish_thread_running_.load()) {
        publish_thread_running_.store(false);
        queue_cv_.notify_all();
        if (publish_thread_.joinable()) {
            publish_thread_.join();
        }
    }
}

bool StreamSession::start()
{
    if (active_.load()) {
        return true;  // Already started
    }

    std::cout << "[StreamSession] Starting - user: " << user_id
              << ", session: " << session_id << std::endl;

    // Start publish thread
    publish_thread_running_.store(true);
    publish_thread_ = std::thread(&StreamSession::publish_thread_func, this);

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

    // Stop publish thread
    if (publish_thread_running_.load()) {
        publish_thread_running_.store(false);
        queue_cv_.notify_all();
        if (publish_thread_.joinable()) {
            publish_thread_.join();
        }
    }

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

    if (!webrtc_peer_) {
        std::cerr << "[StreamSession] No WebRTC peer - ignoring ICE candidate" << std::endl;
        return;
    }

    try {
        auto j = nlohmann::json::parse(payload);
        guint mlineindex = j.value("mlineindex", 0u);
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
    if (!active_.load() && !publish_thread_running_.load()) {
        std::cerr << "[StreamSession] Cannot send signaling - session not active" << std::endl;
        return false;
    }

    std::string topic = topic_from_device("signaling/" + type);
    std::cout << "[StreamSession] Queuing signaling: " << type << std::endl;

    queue_publish(topic, payload);
    return true;
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

void StreamSession::queue_publish(const std::string& topic, const std::string& payload)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        publish_queue_.push({topic, payload});
    }
    queue_cv_.notify_one();
}

void StreamSession::publish_thread_func()
{
    std::cout << "[StreamSession] Publish thread started" << std::endl;

    while (publish_thread_running_.load()) {
        QueuedMessage msg;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !publish_queue_.empty() || !publish_thread_running_.load();
            });

            if (!publish_thread_running_.load() && publish_queue_.empty()) {
                break;
            }

            if (!publish_queue_.empty()) {
                msg = std::move(publish_queue_.front());
                publish_queue_.pop();
            } else {
                continue;
            }
        }

        // Publish outside the lock
        std::cout << "[StreamSession] Publishing: " << msg.topic << std::endl;
        if (mqtt_) {
            mqtt_->publish(msg.topic, msg.payload, 1, false);
            std::cout << "[StreamSession] Published successfully" << std::endl;
        }
    }

    std::cout << "[StreamSession] Publish thread stopped" << std::endl;
}