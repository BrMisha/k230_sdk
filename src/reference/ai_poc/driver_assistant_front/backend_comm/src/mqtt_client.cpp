#include "mqtt_client.h"

#include <mqtt/async_client.h>
#include <mqtt/ssl_options.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <regex>
#include <stdexcept>

#include <openssl/pem.h>
#include <openssl/x509.h>

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
        std::cerr << "[MQTT] Action failed: " << tok.get_message_id() << std::endl;
    }

    void on_success(const mqtt::token& tok) override {
        // Action succeeded
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
        publish_status(false, "", 0);

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

void MqttClient::set_signaling_callback(SignalingCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    signaling_callback_ = std::move(callback);
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

    // Check if it's a command (topic starts with v1/devices/{serial}/to-device/)
    std::string commands_prefix = "v1/devices/" + serial_ + "/to-device/";
    if (topic.find(commands_prefix) == 0) {
        // Parse command JSON: {"command": "...", "request_id": "...", "params": {...}}
        // Simple parsing without external JSON library
        std::string request_id, command, params;

        // Extract "request_id" field
        std::regex id_regex("\"request_id\"\\s*:\\s*\"([^\"]+)\"");
        std::smatch id_match;
        if (std::regex_search(payload, id_match, id_regex)) {
            request_id = id_match[1].str();
        }

        // Extract "command" field
        std::regex cmd_regex("\"command\"\\s*:\\s*\"([^\"]+)\"");
        std::smatch cmd_match;
        if (std::regex_search(payload, cmd_match, cmd_regex)) {
            command = cmd_match[1].str();
        }

        // Extract "params" field (as raw JSON)
        std::regex params_regex("\"params\"\\s*:\\s*(\\{[^}]*\\})");
        std::smatch params_match;
        if (std::regex_search(payload, params_match, params_regex)) {
            params = params_match[1].str();
        } else {
            params = "{}";
        }

        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (command_callback_ && !request_id.empty() && !command.empty()) {
            command_callback_(request_id, command, params);
        }
        return;
    }

    // Check if it's a signaling message
    // Topic format: v1/sessions/{serial}/{user_id}/{session_id}/signaling/from-client[/...]
    std::regex signaling_regex("v1/sessions/[^/]+/([^/]+)/([^/]+)/signaling/from-client(/.*)?");
    std::smatch signaling_match;
    if (std::regex_match(topic, signaling_match, signaling_regex)) {
        std::string user_id = signaling_match[1].str();
        std::string session_id = signaling_match[2].str();

        // Extract message type from payload
        std::regex msg_type_regex("\"type\"\\s*:\\s*\"([^\"]+)\"");
        std::smatch msg_type_match;
        std::string msg_type;
        if (std::regex_search(payload, msg_type_match, msg_type_regex)) {
            msg_type = msg_type_match[1].str();
        }

        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (signaling_callback_) {
            signaling_callback_(user_id, session_id, msg_type, payload);
        }
        return;
    }
}

bool MqttClient::publish(const std::string& topic, const std::string& payload,
                         int qos, bool retained) {
    if (!is_connected()) {
        std::cerr << "[MQTT] Cannot publish: not connected" << std::endl;
        return false;
    }

    try {
        auto msg = mqtt::make_message(topic, payload);
        msg->set_qos(qos);
        msg->set_retained(retained);
        client_->publish(msg);
        return true;

    } catch (const mqtt::exception& e) {
        std::cerr << "[MQTT] Publish failed: " << e.what() << std::endl;
        return false;
    }
}

bool MqttClient::subscribe(const std::string& topic, int qos) {
    if (!client_) {
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

bool MqttClient::subscribe_signaling(const std::string& user_id, const std::string& session_id) {
    return subscribe(topic_signaling_from_client(user_id, session_id), config_.qos_commands);
}

bool MqttClient::unsubscribe_signaling(const std::string& user_id, const std::string& session_id) {
    if (!client_) {
        return false;
    }

    try {
        client_->unsubscribe(topic_signaling_from_client(user_id, session_id));
        return true;
    } catch (const mqtt::exception& e) {
        std::cerr << "[MQTT] Unsubscribe failed: " << e.what() << std::endl;
        return false;
    }
}

bool MqttClient::publish_signaling(const std::string& user_id, const std::string& session_id,
                                   const std::string& message_type,
                                   const std::string& payload) {
    // Payload should already be the full JSON message
    return publish(topic_signaling_from_device(user_id, session_id), payload,
                   config_.qos_commands, false);
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

std::string MqttClient::topic_signaling_from_client(const std::string& user_id, const std::string& session_id) const {
    // Device subscribes to from-client (messages from mobile app)
    // v1/sessions/{serial}/{user_id}/{session_id}/signaling/from-client/#
    return "v1/sessions/" + serial_ + "/" + user_id + "/" + session_id + "/signaling/from-client/#";
}

std::string MqttClient::topic_signaling_from_device(const std::string& user_id, const std::string& session_id) const {
    // v1/sessions/{serial}/{user_id}/{session_id}/signaling/from-device
    return "v1/sessions/" + serial_ + "/" + user_id + "/" + session_id + "/signaling/from-device";
}

} // namespace backend_comm