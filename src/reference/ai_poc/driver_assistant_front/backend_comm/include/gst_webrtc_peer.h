#pragma once

#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <gst/app/gstappsrc.h>
#include <functional>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>

struct IceServer {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
};

class GstWebRTCPeer {
public:
    using OnLocalDescription = std::function<void(const std::string& type, const std::string& sdp)>;
    using OnIceCandidate = std::function<void(guint mlineindex, const std::string& candidate)>;
    using OnStateChange = std::function<void(bool connected)>;
    using OnDataChannelMessage = std::function<void(const std::string& message)>;

    explicit GstWebRTCPeer(const std::vector<IceServer>& ice_servers,
                           int video_width = 1920, int video_height = 1080);
    ~GstWebRTCPeer();

    // Non-copyable
    GstWebRTCPeer(const GstWebRTCPeer&) = delete;
    GstWebRTCPeer& operator=(const GstWebRTCPeer&) = delete;

    // Set callbacks
    void set_on_local_description(OnLocalDescription cb) { on_local_description_ = std::move(cb); }
    void set_on_ice_candidate(OnIceCandidate cb) { on_ice_candidate_ = std::move(cb); }
    void set_on_state_change(OnStateChange cb) { on_state_change_ = std::move(cb); }
    void set_on_data_channel_message(OnDataChannelMessage cb) { on_data_channel_message_ = std::move(cb); }

    // Signaling
    void create_offer();
    void set_remote_description(const std::string& type, const std::string& sdp);
    void add_ice_candidate(guint mlineindex, const std::string& candidate);

    // Video streaming
    void push_video_frame(const uint8_t* data, size_t size, uint64_t pts_us, bool is_keyframe);

    // Data channel
    void send_data(const std::string& message);

    bool is_connected() const { return connected_.load(); }

private:
    void setup_pipeline();
    void setup_webrtc_signals();

    // GStreamer signal handlers (static callbacks)
    static void on_negotiation_needed(GstElement* webrtc, gpointer user_data);
    static void on_ice_candidate(GstElement* webrtc, guint mlineindex,
                                  gchar* candidate, gpointer user_data);
    static void on_ice_connection_state_notify(GstElement* webrtc, GParamSpec* pspec,
                                                gpointer user_data);
    static void on_ice_gathering_state_notify(GstElement* webrtc, GParamSpec* pspec,
                                               gpointer user_data);
    static void on_data_channel(GstElement* webrtc, GObject* channel, gpointer user_data);
    static void on_data_channel_open(GstWebRTCDataChannel* channel, gpointer user_data);
    static void on_data_channel_message(GstWebRTCDataChannel* channel, gchar* message,
                                         gpointer user_data);

    // Promise callbacks for offer/answer
    static void on_offer_created(GstPromise* promise, gpointer user_data);

    GstElement* pipeline_ = nullptr;
    GstElement* webrtc_ = nullptr;
    GstElement* appsrc_ = nullptr;
    GstWebRTCDataChannel* data_channel_ = nullptr;

    std::vector<IceServer> ice_servers_;
    int video_width_;
    int video_height_;
    std::atomic<bool> connected_{false};
    uint64_t frame_count_ = 0;

    // Callbacks
    OnLocalDescription on_local_description_;
    OnIceCandidate on_ice_candidate_;
    OnStateChange on_state_change_;
    OnDataChannelMessage on_data_channel_message_;
};
