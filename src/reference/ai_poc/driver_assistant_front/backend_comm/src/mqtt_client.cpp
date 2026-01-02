#include "mqtt_client.h"
#include "stream_session.h"

#include <mqtt/async_client.h>
#include <mqtt/ssl_options.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <regex>
#include <stdexcept>
#include <algorithm>

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <nlohmann/json.hpp>
#include <thread>

// Stub for Paho MQTT C Log function (missing when PAHO_HIGH_PERFORMANCE=TRUE)
extern "C" {
    void Log(int log_level, int msgno, const char* format, ...) {
        // Intentionally empty - logging disabled for performance
    }
}

namespace backend_comm {

// Internal callback handler that bridges Paho callbacks to MqttClient
class MqttClient::CallbackHandler : public mqtt::callback, public mqtt::iaction_listener {
public:
    explicit CallbackHandler(MqttClient& client) : client_(client) {}

    // mqtt::callback interface
    void connected(const std::string& cause) override {
        std::cout << "[MQTT] Connected: " << cause << std::endl;
        client_.on_connected();
    }

    void connection_lost(const std::string& cause) override {
        std::cout << "[MQTT] Connection lost: " << cause << std::endl;
        client_.on_connection_lost(cause);
    }

    void message_arrived(mqtt::const_message_ptr msg) override {
        client_.on_message(msg->get_topic(), msg->to_string());
    }

    void delivery_complete(mqtt::delivery_token_ptr tok) override {
        // Optional: track delivery confirmations
    }

    // mqtt::iaction_listener interface (for async operations)
    void on_failure(const mqtt::token& tok) override {
        std::cerr << "[MQTT] Action failed: msg_id=" << tok.get_message_id() << std::endl;
        if (tok.get_type() == mqtt::token::Type::SUBSCRIBE) {
            std::cerr << "[MQTT] SUBSCRIBE failed!" << std::endl;
        }
    }

    void on_success(const mqtt::token& tok) override {
        if (tok.get_type() == mqtt::token::Type::SUBSCRIBE) {
            std::cout << "[MQTT] SUBSCRIBE success" << std::endl;
        }
    }

private:
    MqttClient& client_;
};

MqttClient::MqttClient(const MqttConfig& config)
    : config_(config)
{
    // Extract serial from certificate CN (throws on error)
    serial_ = extract_serial_from_cert(config_.cert_path);
    std::cout << "[MQTT] Device serial: " << serial_ << std::endl;

    // Create async client
    client_ = std::make_unique<mqtt::async_client>(config_.broker_uri, serial_);

    // Create callback handler
    callback_handler_ = std::make_unique<CallbackHandler>(*this);
    client_->set_callback(*callback_handler_);
}

MqttClient::~MqttClient() {
    disconnect();
}

bool MqttClient::connect() {
    if (connected_) {
        return true;
    }

    try {
        // Build connection options (MQTT 3.1.1)
        mqtt::connect_options conn_opts;
        conn_opts.set_clean_session(true);
        conn_opts.set_keep_alive_interval(config_.keep_alive_sec);
        conn_opts.set_automatic_reconnect(
            config_.reconnect_min_interval_sec,
            config_.reconnect_max_interval_sec
        );

        // Configure TLS if using ssl://
        if (config_.broker_uri.find("ssl://") == 0 ||
            config_.broker_uri.find("mqtts://") == 0) {

            mqtt::ssl_options ssl_opts;
            ssl_opts.set_trust_store(config_.ca_path);
            ssl_opts.set_key_store(config_.cert_path);
            ssl_opts.set_private_key(config_.key_path);
            ssl_opts.set_enable_server_cert_auth(true);

            conn_opts.set_ssl(ssl_opts);
        }

        // Set Last Will Testament (LWT) - offline status
        std::string lwt_topic = topic_from_device("status");
        std::string lwt_payload = "{\"online\":false}";
        mqtt::message_ptr lwt_msg = mqtt::make_message(lwt_topic, lwt_payload);
        lwt_msg->set_qos(config_.qos_status);
        lwt_msg->set_retained(true);
        conn_opts.set_will_message(lwt_msg);

        std::cout << "[MQTT] Connecting to " << config_.broker_uri << "..." << std::endl;

        // Connect synchronously (wait for result)
        auto tok = client_->connect(conn_opts);
        tok->wait();

        return true;

    } catch (const mqtt::exception& e) {
        std::cerr << "[MQTT] Connection failed: " << e.what() << std::endl;
        return false;
    }
}

void MqttClient::disconnect() {
    if (!client_ || !connected_) {
        return;
    }

    try {
        // Publish offline status before disconnecting
        //publish_status(false, "", 0);

        auto tok = client_->disconnect();
        tok->wait();
        connected_ = false;

        std::cout << "[MQTT] Disconnected" << std::endl;

    } catch (const mqtt::exception& e) {
        std::cerr << "[MQTT] Disconnect error: " << e.what() << std::endl;
    }
}

bool MqttClient::is_connected() const {
    // Don't call client_->is_connected() - it can deadlock in Paho C++ 1.1
    // Just use our own connected_ flag which is set in callbacks
    return connected_ && client_;
}

void MqttClient::set_command_callback(CommandCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    command_callback_ = std::move(callback);
}

void MqttClient::set_connection_callback(ConnectionCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    connection_callback_ = std::move(callback);
}

void MqttClient::on_connected() {
    connected_ = true;

    // Subscribe to to-device topic (receives commands from backend)
    subscribe(topic_to_device(), config_.qos_commands);

    // Notify connection callback
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (connection_callback_) {
            connection_callback_(true);
        }
    }
}

void MqttClient::on_connection_lost(const std::string& cause) {
    connected_ = false;

    // Notify connection callback
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (connection_callback_) {
            connection_callback_(false);
        }
    }
}

void MqttClient::on_message(const std::string& topic, const std::string& payload) {
    std::cout << "[MQTT] Message on " << topic << ": " << payload << std::endl;

    // Split topic by "/"
    std::vector<std::string> parts;
    std::istringstream iss(topic);
    std::string part;
    while (std::getline(iss, part, '/')) {
        parts.push_back(part);
    }

    // All topics start with "v1"
    if (parts.size() < 2 || parts[0] != "v1") {
        return;
    }

    // v1/devices/{serial}/to-device/{command}
    // [0]=v1, [1]=devices, [2]=serial, [3]=to-device, [4]=command
    if (parts.size() >= 5 && parts[1] == "devices" &&
        parts[2] == serial_ && parts[3] == "to-device") {

        std::string command = parts[4];

        // Payload is the params directly (or may contain request_id at top level)
        std::string request_id;
        std::string params = payload;
        try {
            auto json = nlohmann::json::parse(payload);
            request_id = json.value("request_id", "");
            if (request_id.empty()) {
                request_id = command;
            }
        } catch (const nlohmann::json::exception& e) {
            std::cerr << "[MQTT] Command JSON parse error: " << e.what() << std::endl;
            request_id = command;
        }

        std::cout << "[MQTT] Command: " << command << " (request_id=" << request_id << ")" << std::endl;

        // Handle stream commands internally
        if (command == "stream_start") {
            handle_stream_start(request_id, params);
            return;
        }
        if (command == "stream_stop") {
            handle_stream_stop(request_id, params);
            return;
        }

        // Pass other commands to external callback
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (command_callback_) {
            command_callback_(request_id, command, params);
        }
    }

    // v1/sessions/{serial}/{user_id}/{session_id}/from-client/{path...}
    // [0]=v1, [1]=sessions, [2]=serial, [3]=user_id, [4]=session_id, [5]=from-client, [6+]=path
    else if (parts.size() >= 7 && parts[1] == "sessions" &&
        parts[2] == serial_ && parts[5] == "from-client") {

        std::string user_id = parts[3];
        std::string session_id = parts[4];

        // Find session and route message
        auto session = find_session(user_id, session_id);
        if (!session) {
            std::cout << "[MQTT] No session for message, ignoring" << std::endl;
            return;
        }

        // Pass path parts after /from-client/ (parts[6], parts[7], ...)
        std::vector<std::string> path_parts(parts.begin() + 6, parts.end());
        session->on_message(path_parts, payload);
    }
}

bool MqttClient::publish(const std::string& topic, const std::string& payload,
                         int qos, bool retained) {
    if (!is_connected()) {
        std::cerr << "[MQTT] Cannot publish: not connected" << std::endl;
        return false;
    }

    // Fire-and-forget async publish to avoid GLib thread deadlock
    // Paho MQTT's publish() blocks when called from GLib main loop thread
    // Note: Using detached thread because std::async's returned future blocks on destruction
    std::thread([this, topic, payload, qos, retained]() {
        try {
            auto msg = mqtt::make_message(topic, payload);
            msg->set_qos(qos);
            msg->set_retained(retained);
            client_->publish(msg);
            std::cout << "[MQTT] Published: " << topic << std::endl;
        } catch (const mqtt::exception& e) {
            std::cerr << "[MQTT] Publish failed: " << e.what() << std::endl;
        }
    }).detach();

    return true;  // Optimistically return success
}

bool MqttClient::subscribe(const std::string& topic, int qos) {
    if (!client_) {
        std::cerr << "[MQTT] Subscribe failed: client is null" << std::endl;
        return false;
    }

    try {
        std::cout << "[MQTT] Subscribing to " << topic << std::endl;
        client_->subscribe(topic, qos);
        return true;

    } catch (const mqtt::exception& e) {
        std::cerr << "[MQTT] Subscribe failed: " << e.what() << std::endl;
        return false;
    }
}

bool MqttClient::publish_status(bool online, const std::string& firmware,
                                int64_t uptime) {
    std::ostringstream json;
    json << R"({"online":)" << (online ? "true" : "false");
    json << R"(,"timestamp":)" << get_timestamp_unix();
    if (uptime > 0) {
        json << R"(,"uptime":)" << uptime;
    }
    if (!firmware.empty()) {
        json << R"(,"firmware":")" << firmware << "\"";
    }
    json << "}";

    return publish(topic_from_device("status"), json.str(), config_.qos_status, true);
}

bool MqttClient::publish_event(const std::string& event_type,
                               const std::string& severity,
                               const std::string& data_json) {
    std::ostringstream json;
    json << R"({"type":")" << event_type << "\"";
    json << R"(,"severity":")" << severity << "\"";
    json << R"(,"timestamp":)" << get_timestamp_unix();
    json << R"(,"data":)" << data_json << "}";

    return publish(topic_from_device("events"), json.str(), config_.qos_events, false);
}

bool MqttClient::publish_command_response(const std::string& request_id,
                                          const std::string& status,
                                          const std::string& data_json) {
    std::ostringstream json;
    json << R"({"request_id":")" << request_id << "\"";
    json << R"(,"status":")" << status << "\"";
    json << R"(,"timestamp":)" << get_timestamp_unix();
    json << R"(,"data":)" << data_json << "}";

    return publish(topic_from_device("commands/response"), json.str(), config_.qos_commands, false);
}

bool MqttClient::unsubscribe(const std::string& topic) {
    if (!client_) {
        return false;
    }

    try {
        std::cout << "[MQTT] Unsubscribing from " << topic << std::endl;
        client_->unsubscribe(topic);
        return true;
    } catch (const mqtt::exception& e) {
        std::cerr << "[MQTT] Unsubscribe failed: " << e.what() << std::endl;
        return false;
    }
}

std::string MqttClient::extract_serial_from_cert(const std::string& cert_path) {
    // Open certificate file
    FILE* fp = fopen(cert_path.c_str(), "r");
    if (!fp) {
        throw std::runtime_error("[MQTT] Cannot open certificate: " + cert_path);
    }

    // Parse PEM certificate
    X509* cert = PEM_read_X509(fp, nullptr, nullptr, nullptr);
    fclose(fp);

    if (!cert) {
        throw std::runtime_error("[MQTT] Failed to parse certificate: " + cert_path);
    }

    // Get subject name and extract CN (Common Name)
    X509_NAME* subject = X509_get_subject_name(cert);
    if (!subject) {
        X509_free(cert);
        throw std::runtime_error("[MQTT] Certificate has no subject: " + cert_path);
    }

    char cn[256] = {0};
    int cn_len = X509_NAME_get_text_by_NID(subject, NID_commonName, cn, sizeof(cn));

    X509_free(cert);

    if (cn_len <= 0) {
        throw std::runtime_error("[MQTT] Certificate has no CN field: " + cert_path);
    }

    return std::string(cn);
}

int64_t MqttClient::get_timestamp_unix() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
}

// Topic helper methods
std::string MqttClient::topic_to_device() const {
    // Device subscribes to this topic to receive commands from backend
    return "v1/devices/" + serial_ + "/to-device/#";
}

std::string MqttClient::topic_from_device(const std::string& subtopic) const {
    // Device publishes to this topic to send messages to backend
    return "v1/devices/" + serial_ + "/from-device/" + subtopic;
}

// Stream session handlers
void MqttClient::handle_stream_start(const std::string& cmd_id, const std::string& params) {
    std::cout << "[MQTT] stream_start params: " << params << std::endl;

    std::string user_id, session_id;
    std::vector<std::string> ice_server_urls;

    try {
        auto json = nlohmann::json::parse(params);
        user_id = json.value("user_id", "");
        session_id = json.value("session_id", "");

        // Parse ICE servers and convert to URL strings for libdatachannel
        // Format: "stun:host:port" or "turn:user:pass@host:port"
        if (json.contains("ice_servers")) {
            for (const auto& server : json["ice_servers"]) {
                std::string username = server.value("username", "");
                std::string credential = server.value("credential", "");

                if (server.contains("urls")) {
                    for (const auto& url : server["urls"]) {
                        std::string url_str = url.get<std::string>();
                        // If TURN server with credentials, embed them in URL
                        if (!username.empty() && !credential.empty() &&
                            (url_str.find("turn:") == 0 || url_str.find("turns:") == 0)) {
                            // Parse scheme and host:port from URL
                            // Handles both "turn:host:port" and "turn://host:port"
                            size_t scheme_end = url_str.find(':');
                            if (scheme_end != std::string::npos) {
                                std::string scheme = url_str.substr(0, scheme_end);  // "turn" or "turns"
                                std::string rest = url_str.substr(scheme_end + 1);
                                // Remove leading "//" if present
                                if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
                                    rest = rest.substr(2);
                                }
                                // Build URL: turn:user:pass@host:port
                                url_str = scheme + ":" + username + ":" + credential + "@" + rest;
                            }
                        }
                        std::cout << "[MQTT] ICE server: " << url_str << std::endl;
                        ice_server_urls.push_back(url_str);
                    }
                }
            }
            std::cout << "[MQTT] Parsed " << ice_server_urls.size() << " ICE server URLs" << std::endl;
        }
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[MQTT] stream_start JSON parse error: " << e.what() << std::endl;
        publish_command_response(cmd_id, "error",
            R"({"error":"invalid_json","message":")" + std::string(e.what()) + R"("})");
        return;
    }

    if (user_id.empty() || session_id.empty()) {
        std::cerr << "[MQTT] stream_start missing user_id or session_id" << std::endl;
        publish_command_response(cmd_id, "error",
            R"({"error":"missing_params","message":"user_id and session_id required"})");
        return;
    }

    // Check if session already exists
    if (find_session(user_id, session_id)) {
        std::cout << "[MQTT] Session already exists for user: " << user_id << std::endl;
        publish_command_response(cmd_id, "success");
        return;
    }

    // Create new session with ICE server URLs
    // Note: We need shared_from_this, but MqttClient doesn't inherit from enable_shared_from_this
    // So we pass 'this' wrapped in a shared_ptr with a no-op deleter for now
    // This is safe because sessions_ is owned by MqttClient and cleaned up before destruction
    auto session = std::make_shared<StreamSession>(
        std::shared_ptr<MqttClient>(this, [](MqttClient*){}),  // non-owning shared_ptr
        user_id, session_id, std::move(ice_server_urls));

    if (session->start()) {
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.push_back(session);
        }
        publish_command_response(cmd_id, "success");
    } else {
        publish_command_response(cmd_id, "error", R"({"error":"session_failed"})");
    }
}

void MqttClient::handle_stream_stop(const std::string& cmd_id, const std::string& params) {
    std::cout << "[MQTT] stream_stop params: " << params << std::endl;

    std::string user_id, session_id;
    try {
        auto json = nlohmann::json::parse(params);
        user_id = json.value("user_id", "");
        session_id = json.value("session_id", "");
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[MQTT] stream_stop JSON parse error: " << e.what() << std::endl;
        publish_command_response(cmd_id, "error",
            R"({"error":"invalid_json","message":")" + std::string(e.what()) + R"("})");
        return;
    }

    if (user_id.empty() || session_id.empty()) {
        std::cerr << "[MQTT] stream_stop missing user_id or session_id" << std::endl;
        publish_command_response(cmd_id, "error",
            R"({"error":"missing_params","message":"user_id and session_id required"})");
        return;
    }

    remove_session(user_id, session_id);
    publish_command_response(cmd_id, "success");
}

std::shared_ptr<StreamSession> MqttClient::find_session(const std::string& user_id, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = std::find_if(sessions_.begin(), sessions_.end(),
        [&](const auto& s) { return s->user_id == user_id && s->session_id == session_id; });
    return (it != sessions_.end()) ? *it : nullptr;
}

void MqttClient::remove_session(const std::string& user_id, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = std::find_if(sessions_.begin(), sessions_.end(),
        [&](const auto& s) { return s->user_id == user_id && s->session_id == session_id; });
    if (it != sessions_.end()) {
        (*it)->stop();
        sessions_.erase(it);
        std::cout << "[MQTT] Removed session for user: " << user_id << std::endl;
    }
}

void MqttClient::push_video_to_sessions(const uint8_t* data, size_t size, uint64_t pts_us, uint8_t type) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& session : sessions_) {
        if (session && session->is_active()) {
            session->push_video_frame(data, size, pts_us, type);
        }
    }
}

} // namespace backend_comm