#include "websocket_server.h"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <cstdio>

namespace websocket_server {

// Per-session data for WebSocket connections
struct session_data {
    bool initialized;
};

// Global pointer to server instance for static callbacks
static WebSocketServer* g_server_instance = nullptr;

// Protocol definitions
static struct lws_protocols protocols[] = {
    {
        "http",
        WebSocketServer::callback_http,
        0,
        0,
    },
    {
        "detection-protocol",
        WebSocketServer::callback_ws,
        sizeof(struct session_data),
        1024,
    },
    { nullptr, nullptr, 0, 0 } // terminator
};

WebSocketServer::WebSocketServer(int port, const char* www_path)
    : context_(nullptr)
    , port_(port)
    , www_path_(www_path)
    , running_(false)
    , has_pending_broadcast_(false)
{
    g_server_instance = this;
}

WebSocketServer::~WebSocketServer() {
    stop();
    g_server_instance = nullptr;
}

void WebSocketServer::run() {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = port_;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    printf("WebSocket: Creating context on port %d...\n", port_);
    context_ = lws_create_context(&info);
    if (!context_) {
        printf("WebSocket: Failed to create context\n");
        return;
    }

    printf("WebSocket: Server started successfully on port %d\n", port_);
    printf("WebSocket: Waiting for connections...\n");
    running_ = true;

    while (running_) {
        lws_service(context_, 50);
    }

    lws_context_destroy(context_);
    context_ = nullptr;
    printf("WebSocket: Server stopped\n");
}

void WebSocketServer::stop() {
    running_ = false;
}

void WebSocketServer::broadcast_detections(
    uint64_t pts,
    const driver_assistant_detector::DetectedSituation& situation,
    const std::vector<driver_assistant_detector::DetectionNormalizedCommon>& detections)
{
    std::lock_guard<std::mutex> lock(broadcast_mutex_);
    printf("WebSocket: Detected %d %lu\n", situation.color, context_);

    broadcast_buffer_ = create_json_message(pts, situation, detections);
    has_pending_broadcast_ = true;

    if (context_) {
        lws_callback_on_writable_all_protocol(context_, &protocols[1]);
    }
}

std::string WebSocketServer::create_json_message(
    uint64_t pts,
    const driver_assistant_detector::DetectedSituation& situation,
    const std::vector<driver_assistant_detector::DetectionNormalizedCommon>& detections)
{
    std::ostringstream oss;

    oss << "{";
    oss << "\"timestamp\":" << pts << ",";

    // Situation
    oss << "\"situation\":{";
    oss << "\"color\":\"" << driver_assistant_detector::DetectedSituationColor_str[situation.color] << "\",";
    oss << "\"arrow_left\":" << (situation.arrow_left ? "true" : "false") << ",";
    oss << "\"arrow_right\":" << (situation.arrow_right ? "true" : "false") << ",";
    oss << "\"arrow_forward\":" << (situation.arrow_forward ? "true" : "false");
    oss << "},";

    // Detections array
    oss << "\"detections\":[";
    for (size_t i = 0; i < detections.size(); i++) {
        const auto& det = detections[i];
        oss << "{";
        oss << "\"class_id\":" << det.class_id << ",";
        oss << "\"class_name\":\"" << driver_assistant_detector::detect_classes_str[det.class_id] << "\",";
        oss << "\"confidence\":" << std::fixed << std::setprecision(4) << det.confidence << ",";
        oss << "\"x\":" << std::fixed << std::setprecision(4) << det.x << ",";
        oss << "\"y\":" << std::fixed << std::setprecision(4) << det.y << ",";
        oss << "\"w\":" << std::fixed << std::setprecision(4) << det.w << ",";
        oss << "\"h\":" << std::fixed << std::setprecision(4) << det.h;
        oss << "}";
        if (i < detections.size() - 1) {
            oss << ",";
        }
    }
    oss << "]";

    oss << "}";

    return oss.str();
}

int WebSocketServer::callback_http(struct lws* wsi, enum lws_callback_reasons reason,
                                    void* user, void* in, size_t len)
{
    switch (reason) {
        case LWS_CALLBACK_HTTP: {
            const char* requested = (const char*)in;

            // Serve index.html for root path
            if (strcmp(requested, "/") == 0) {
                if (lws_serve_http_file(wsi, "www/index.html", "text/html", nullptr, 0) < 0) {
                    return -1;
                }
            } else {
                // Serve other files from www directory
                char filepath[256];
                snprintf(filepath, sizeof(filepath), "www%s", requested);

                const char* mime_type = "text/html";
                if (strstr(requested, ".js")) mime_type = "application/javascript";
                else if (strstr(requested, ".css")) mime_type = "text/css";

                if (lws_serve_http_file(wsi, filepath, mime_type, nullptr, 0) < 0) {
                    return -1;
                }
            }
            break;
        }

        case LWS_CALLBACK_HTTP_WRITEABLE:
            // File serving complete
            break;

        default:
            break;
    }

    return 0;
}

int WebSocketServer::callback_ws(struct lws* wsi, enum lws_callback_reasons reason,
                                  void* user, void* in, size_t len)
{
    struct session_data* session = (struct session_data*)user;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            printf("WebSocket: Client connected\n");
            session->initialized = true;
            break;

        case LWS_CALLBACK_CLOSED:
            printf("WebSocket: Client disconnected\n");
            session->initialized = false;
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            if (!g_server_instance || !session->initialized) {
                break;
            }

            std::lock_guard<std::mutex> lock(g_server_instance->broadcast_mutex_);

            if (g_server_instance->has_pending_broadcast_) {
                const std::string& msg = g_server_instance->broadcast_buffer_;

                // Allocate buffer with LWS_PRE padding
                size_t msg_len = msg.length();
                unsigned char* buf = new unsigned char[LWS_PRE + msg_len];

                memcpy(&buf[LWS_PRE], msg.c_str(), msg_len);

                int result = lws_write(wsi, &buf[LWS_PRE], msg_len, LWS_WRITE_TEXT);

                delete[] buf;

                if (result < 0) {
                    printf("WebSocket: Write error\n");
                    return -1;
                }
            }
            break;
        }

        case LWS_CALLBACK_RECEIVE:
            // Client sent data (we can ignore for now, or handle commands later)
            break;

        default:
            break;
    }

    return 0;
}

} // namespace websocket_server