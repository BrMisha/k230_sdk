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

    // Subscribe to signaling topic from app
    if (!mqtt_->subscribe_signaling(user_id, session_id)) {
        std::cerr << "[StreamSession] Failed to subscribe to signaling topic" << std::endl;
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

    // Unsubscribe from signaling topic
    mqtt_->unsubscribe_signaling(user_id, session_id);

    active_.store(false);
    std::cout << "[StreamSession] Stopped" << std::endl;
}

void StreamSession::on_message(const std::string& type, const std::string& payload)
{
    if (!active_.load()) {
        std::cout << "[StreamSession] Ignoring message - session not active" << std::endl;
        return;
    }

    std::cout << "[StreamSession] Received message - type: " << type << std::endl;

    if (type == "watch") {
        handle_watch(payload);
    } else if (type == "answer") {
        handle_answer(payload);
    } else if (type == "ice") {
        handle_ice(payload);
    } else if (type == "stop") {
        handle_stop(payload);
    } else {
        std::cout << "[StreamSession] Unknown message type: " << type << std::endl;
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
    return mqtt_->publish_signaling(user_id, session_id, type, payload);
}