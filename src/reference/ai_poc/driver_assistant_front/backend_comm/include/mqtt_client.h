#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <vector>

// Forward declarations
namespace mqtt {
    class async_client;
    class connect_options;
}

class StreamSession;

namespace backend_comm {

struct MqttConfig {
    std::string broker_uri;          // e.g., "ssl://emqx.example.com:8883"
    std::string client_id;           // Device serial number

    // Certificate paths (relative to working directory)
    std::string cert_path = "cert/device.pem";
    std::string key_path = "cert/device.key";
    std::string ca_path = "cert/ca.pem";

    // Reconnect settings
    int reconnect_min_interval_sec = 1;
    int reconnect_max_interval_sec = 60;

    // Keep alive
    int keep_alive_sec = 60;

    // QoS levels
    int qos_status = 1;      // Retained status messages
    int qos_telemetry = 0;   // Fire-and-forget telemetry
    int qos_events = 1;      // Important events
    int qos_commands = 1;    // Commands need delivery guarantee
};

// Callback types
using CommandCallback = std::function<void(const std::string& request_id,
                                            const std::string& command,
                                            const std::string& params_json)>;
using ConnectionCallback = std::function<void(bool connected)>;

class MqttClient {
public:
    explicit MqttClient(const MqttConfig& config);
    ~MqttClient();

    // Non-copyable
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    // Connection management
    bool connect();
    void disconnect();
    bool is_connected() const;

    // Set callbacks (command_callback only receives non-stream commands)
    void set_command_callback(CommandCallback callback);
    void set_connection_callback(ConnectionCallback callback);

    // Publishing methods
    bool publish_status(bool online, const std::string& firmware,
                       int64_t uptime);
    bool publish_event(const std::string& event_type,
                      const std::string& severity,
                      const std::string& data_json);
    bool publish_command_response(const std::string& request_id,
                                  const std::string& status,
                                  const std::string& data_json = "{}");

    // WebRTC signaling (topic: v1/sessions/{serial}/{user_id}/{session_id}/signaling/...)
    bool subscribe_signaling(const std::string& user_id, const std::string& session_id);
    bool unsubscribe_signaling(const std::string& user_id, const std::string& session_id);
    bool publish_signaling(const std::string& user_id, const std::string& session_id,
                          const std::string& message_type,
                          const std::string& payload);

    // Get device serial (extracted from certificate CN)
    const std::string& get_serial() const { return serial_; }

private:
    class CallbackHandler;

    void on_connected();
    void on_connection_lost(const std::string& cause);
    void on_message(const std::string& topic, const std::string& payload);

    bool publish(const std::string& topic, const std::string& payload,
                int qos, bool retained = false);
    bool subscribe(const std::string& topic, int qos);

    std::string extract_serial_from_cert(const std::string& cert_path);
    int64_t get_timestamp_unix();

    // Topic helpers
    std::string topic_to_device() const;  // Device subscribes: v1/devices/{serial}/to-device/#
    std::string topic_from_device(const std::string& subtopic) const;  // Device publishes: v1/devices/{serial}/from-device/{subtopic}
    std::string topic_signaling_from_client(const std::string& user_id, const std::string& session_id) const;
    std::string topic_signaling_from_device(const std::string& user_id, const std::string& session_id) const;

    // Stream session handlers
    void handle_stream_start(const std::string& cmd_id, const std::string& params);
    void handle_stream_stop(const std::string& cmd_id, const std::string& params);
    std::shared_ptr<StreamSession> find_session(const std::string& user_id, const std::string& session_id);
    void remove_session(const std::string& user_id, const std::string& session_id);

    MqttConfig config_;
    std::string serial_;

    std::unique_ptr<mqtt::async_client> client_;
    std::unique_ptr<CallbackHandler> callback_handler_;

    std::atomic<bool> connected_{false};

    std::mutex callback_mutex_;
    CommandCallback command_callback_;
    ConnectionCallback connection_callback_;

    // Stream sessions (managed internally)
    std::vector<std::shared_ptr<StreamSession>> sessions_;
};

} // namespace backend_comm