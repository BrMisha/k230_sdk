#include "stream_session.h"
#include <iostream>

StreamSession::StreamSession(std::shared_ptr<backend_comm::MqttClient> mqtt,
                             std::string user_id,
                             std::string session_id)
    : user_id(std::move(user_id))
    , session_id(std::move(session_id))
    , mqtt_(std::move(mqtt))
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
    std::cout << "[StreamSession] Payload: " << payload << std::endl;

    // TODO: Generate SDP offer and send it
    std::cout << "[StreamSession] TODO: Generate SDP offer and send 'offer' message" << std::endl;
}

void StreamSession::handle_answer(const std::string& payload)
{
    std::cout << "[StreamSession] Received 'answer' - app sent SDP answer" << std::endl;
    std::cout << "[StreamSession] Payload: " << payload << std::endl;

    // TODO: Apply remote SDP answer
    std::cout << "[StreamSession] TODO: Apply remote SDP answer" << std::endl;
}

void StreamSession::handle_ice(const std::string& payload)
{
    std::cout << "[StreamSession] Received 'ice' - ICE candidate from app" << std::endl;
    std::cout << "[StreamSession] Payload: " << payload << std::endl;

    // TODO: Add remote ICE candidate
    std::cout << "[StreamSession] TODO: Add remote ICE candidate" << std::endl;
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

    std::cout << "[StreamSession] Sending signaling - type: " << type << std::endl;
    return mqtt_->publish(topic_from_device("signaling/" + type), payload, 1, false);
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