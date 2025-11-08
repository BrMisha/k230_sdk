#ifndef WEBSOCKET_SERVER_H
#define WEBSOCKET_SERVER_H

#include <libwebsockets.h>
#include <vector>
#include <mutex>
#include "../../driver_assistant_detector/common.h"

namespace websocket_server {

class WebSocketServer {
public:
    WebSocketServer(int port, const char* www_path);
    ~WebSocketServer();

    // Start the server in the current thread (blocking)
    void run();

    // Stop the server
    void stop();

    // Broadcast detection data to all connected clients
    void broadcast_detections(
        uint64_t pts,
        const driver_assistant_detector::DetectedSituation& situation,
        const std::vector<driver_assistant_detector::DetectionNormalizedCommon>& detections
    );

    // Static callbacks for libwebsockets (must be public)
    static int callback_http(struct lws* wsi, enum lws_callback_reasons reason,
                             void* user, void* in, size_t len);

    static int callback_ws(struct lws* wsi, enum lws_callback_reasons reason,
                          void* user, void* in, size_t len);

private:
    struct lws_context* context_;
    int port_;
    const char* www_path_;
    volatile bool running_;

    // Mutex for thread-safe broadcasting
    std::mutex broadcast_mutex_;

    // Buffer for broadcast message
    std::string broadcast_buffer_;
    bool has_pending_broadcast_;

    // Helper to create JSON message
    std::string create_json_message(
        uint64_t pts,
        const driver_assistant_detector::DetectedSituation& situation,
        const std::vector<driver_assistant_detector::DetectionNormalizedCommon>& detections
    );
};

} // namespace websocket_server

#endif // WEBSOCKET_SERVER_H