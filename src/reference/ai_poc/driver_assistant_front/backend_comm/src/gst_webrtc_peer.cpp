#include "gst_webrtc_peer.h"
#include <iostream>
#include <sstream>

GstWebRTCPeer::GstWebRTCPeer(const std::vector<IceServer>& ice_servers,
                               int video_width, int video_height)
    : ice_servers_(ice_servers)
    , video_width_(video_width)
    , video_height_(video_height)
{
    setup_pipeline();
}

GstWebRTCPeer::~GstWebRTCPeer()
{
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

void GstWebRTCPeer::setup_pipeline()
{
    std::cout << "[WebRTC] setup_pipeline() starting..." << std::endl;
    std::cout.flush();

    // Create pipeline
    pipeline_ = gst_pipeline_new("webrtc-pipeline");
    if (!pipeline_) {
        std::cerr << "[WebRTC] Failed to create pipeline" << std::endl;
        return;
    }

    // Create elements: appsrc -> h265parse -> rtph265pay -> webrtcbin
    appsrc_ = gst_element_factory_make("appsrc", "video-source");
    GstElement* h265parse = gst_element_factory_make("h265parse", "h265-parse");
    GstElement* rtppay = gst_element_factory_make("rtph265pay", "rtp-pay");
    webrtc_ = gst_element_factory_make("webrtcbin", "webrtc");

    if (!appsrc_ || !h265parse || !rtppay || !webrtc_) {
        std::cerr << "[WebRTC] Failed to create elements:" << std::endl;
        std::cerr << "  appsrc: " << (appsrc_ ? "OK" : "FAILED") << std::endl;
        std::cerr << "  h265parse: " << (h265parse ? "OK" : "FAILED") << std::endl;
        std::cerr << "  rtph265pay: " << (rtppay ? "OK" : "FAILED") << std::endl;
        std::cerr << "  webrtcbin: " << (webrtc_ ? "OK" : "FAILED") << std::endl;
        if (pipeline_) gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return;
    }
    std::cout << "[WebRTC] All elements created successfully" << std::endl;

    // Configure appsrc for H.265 byte-stream
    GstCaps* caps = gst_caps_new_simple("video/x-h265",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        "width", G_TYPE_INT, video_width_,
        "height", G_TYPE_INT, video_height_,
        NULL);
    g_object_set(G_OBJECT(appsrc_),
        "caps", caps,
        "format", GST_FORMAT_TIME,
        "is-live", TRUE,
        "do-timestamp", FALSE,
        NULL);
    gst_caps_unref(caps);

    // Configure RTP payloader
    g_object_set(G_OBJECT(rtppay),
        "config-interval", 1,  // Send SPS/PPS with every IDR
        "pt", 96,              // Payload type
        NULL);

    // Configure webrtcbin
    g_object_set(G_OBJECT(webrtc_),
        "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
        NULL);

    // Set STUN/TURN servers
    for (const auto& server : ice_servers_) {
        for (const auto& url : server.urls) {
            if (url.find("stun:") == 0) {
                g_object_set(G_OBJECT(webrtc_), "stun-server", url.c_str(), NULL);
                std::cout << "[WebRTC] Set STUN server: " << url << std::endl;
            } else if (url.find("turn:") == 0) {
                std::string turn_url = url;
                if (!server.username.empty() && !server.credential.empty()) {
                    size_t pos = url.find("turn:");
                    if (pos != std::string::npos) {
                        turn_url = "turn://" + server.username + ":" +
                                   server.credential + "@" + url.substr(5);
                    }
                }
                g_object_set(G_OBJECT(webrtc_), "turn-server", turn_url.c_str(), NULL);
                std::cout << "[WebRTC] Set TURN server: " << url << std::endl;
            }
        }
    }

    // Add elements to pipeline
    gst_bin_add_many(GST_BIN(pipeline_), appsrc_, h265parse, rtppay, webrtc_, NULL);

    // Link appsrc -> h265parse -> rtph265pay
    if (!gst_element_link_many(appsrc_, h265parse, rtppay, NULL)) {
        std::cerr << "[WebRTC] Failed to link appsrc -> h265parse -> rtppay" << std::endl;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return;
    }

    // Get RTP pad from rtppay and link to webrtcbin
    GstPad* rtp_src_pad = gst_element_get_static_pad(rtppay, "src");
    GstPad* webrtc_sink_pad = gst_element_get_request_pad(webrtc_, "sink_%u");
    if (gst_pad_link(rtp_src_pad, webrtc_sink_pad) != GST_PAD_LINK_OK) {
        std::cerr << "[WebRTC] Failed to link rtppay to webrtcbin" << std::endl;
    } else {
        std::cout << "[WebRTC] Linked video pipeline to webrtcbin" << std::endl;
    }
    gst_object_unref(rtp_src_pad);
    gst_object_unref(webrtc_sink_pad);

    setup_webrtc_signals();

    // Set pipeline to READY
    std::cout << "[WebRTC] Setting pipeline to READY..." << std::endl;
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_READY);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[WebRTC] Failed to set pipeline to READY" << std::endl;
        GstBus* bus = gst_element_get_bus(pipeline_);
        GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (msg) {
            GError* err = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            std::cerr << "[WebRTC] Error: " << (err ? err->message : "unknown") << std::endl;
            if (debug) {
                std::cerr << "[WebRTC] Debug: " << debug << std::endl;
                g_free(debug);
            }
            if (err) g_error_free(err);
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
    } else {
        std::cout << "[WebRTC] Pipeline in READY state" << std::endl;
    }
}

void GstWebRTCPeer::setup_webrtc_signals()
{
    // Connect signals
    g_signal_connect(webrtc_, "on-negotiation-needed",
        G_CALLBACK(on_negotiation_needed), this);
    g_signal_connect(webrtc_, "on-ice-candidate",
        G_CALLBACK(on_ice_candidate), this);
    g_signal_connect(webrtc_, "notify::ice-connection-state",
        G_CALLBACK(on_ice_connection_state_notify), this);
    g_signal_connect(webrtc_, "notify::ice-gathering-state",
        G_CALLBACK(on_ice_gathering_state_notify), this);
    g_signal_connect(webrtc_, "on-data-channel",
        G_CALLBACK(on_data_channel), this);
}

void GstWebRTCPeer::create_offer()
{
    if (!webrtc_) {
        std::cerr << "[WebRTC] Cannot create offer - webrtcbin not initialized" << std::endl;
        return;
    }

    std::cout << "[WebRTC] create_offer() called - setting pipeline to PLAYING" << std::endl;
    std::cout.flush();

    // Set pipeline to PLAYING
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    std::cout << "[WebRTC] set_state(PLAYING) returned " << ret << std::endl;
    std::cout.flush();

    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[WebRTC] Failed to set pipeline to PLAYING" << std::endl;
        return;
    }

    // Create offer synchronously (no GLib main loop available)
    std::cout << "[WebRTC] Creating offer..." << std::endl;
    std::cout.flush();

    GstPromise* promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_, "create-offer", NULL, promise);

    // Wait for offer to be created
    gst_promise_wait(promise);

    const GstStructure* reply = gst_promise_get_reply(promise);
    if (!reply) {
        std::cerr << "[WebRTC] Failed to get promise reply" << std::endl;
        gst_promise_unref(promise);
        return;
    }

    GstWebRTCSessionDescription* offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);

    if (!offer) {
        std::cerr << "[WebRTC] Failed to create offer" << std::endl;
        gst_promise_unref(promise);
        return;
    }

    // Set local description
    GstPromise* local_promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_, "set-local-description", offer, local_promise);
    gst_promise_wait(local_promise);
    gst_promise_unref(local_promise);

    // Get SDP text
    gchar* sdp_text = gst_sdp_message_as_text(offer->sdp);
    std::string sdp_str(sdp_text);
    g_free(sdp_text);

    std::cout << "[WebRTC] Created offer with video" << std::endl;
    std::cout.flush();

    // Notify callback
    if (on_local_description_) {
        on_local_description_("offer", sdp_str);
    }

    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);
}

void GstWebRTCPeer::set_remote_description(const std::string& type, const std::string& sdp)
{
    if (!webrtc_) {
        std::cerr << "[WebRTC] Cannot set remote description - webrtcbin not initialized" << std::endl;
        return;
    }

    std::cout << "[WebRTC] Parsing SDP..." << std::endl;
    std::cout.flush();

    GstSDPMessage* sdp_msg;
    GstSDPResult result = gst_sdp_message_new(&sdp_msg);
    if (result != GST_SDP_OK) {
        std::cerr << "[WebRTC] Failed to create SDP message" << std::endl;
        return;
    }

    result = gst_sdp_message_parse_buffer(
        reinterpret_cast<const guint8*>(sdp.c_str()), sdp.size(), sdp_msg);
    if (result != GST_SDP_OK) {
        std::cerr << "[WebRTC] Failed to parse SDP" << std::endl;
        gst_sdp_message_free(sdp_msg);
        return;
    }

    std::cout << "[WebRTC] Creating session description..." << std::endl;
    std::cout.flush();

    GstWebRTCSDPType sdp_type = (type == "answer") ?
        GST_WEBRTC_SDP_TYPE_ANSWER : GST_WEBRTC_SDP_TYPE_OFFER;

    GstWebRTCSessionDescription* desc =
        gst_webrtc_session_description_new(sdp_type, sdp_msg);

    std::cout << "[WebRTC] Emitting set-remote-description..." << std::endl;
    std::cout.flush();

    // Use synchronous wait like create_offer
    GstPromise* promise = gst_promise_new();
    g_signal_emit_by_name(webrtc_, "set-remote-description", desc, promise);
    gst_promise_wait(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(desc);

    std::cout << "[WebRTC] Set remote " << type << std::endl;
    std::cout.flush();
}

void GstWebRTCPeer::add_ice_candidate(guint mlineindex, const std::string& candidate)
{
    if (!webrtc_) {
        std::cerr << "[WebRTC] Cannot add ICE candidate - webrtcbin not initialized" << std::endl;
        return;
    }

    g_signal_emit_by_name(webrtc_, "add-ice-candidate", mlineindex, candidate.c_str());
    std::cout << "[WebRTC] Added ICE candidate" << std::endl;
}

void GstWebRTCPeer::push_video_frame(const uint8_t* data, size_t size,
                                       uint64_t pts_us, bool is_keyframe)
{
    if (!appsrc_) {
        std::cerr << "[WebRTC] Cannot push video - appsrc not initialized" << std::endl;
        return;
    }

    // Create buffer with copy of data
    GstBuffer* buffer = gst_buffer_new_allocate(NULL, size, NULL);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        memcpy(map.data, data, size);
        gst_buffer_unmap(buffer, &map);
    }

    // Set timestamps (convert microseconds to nanoseconds)
    GST_BUFFER_PTS(buffer) = pts_us * 1000;
    GST_BUFFER_DTS(buffer) = pts_us * 1000;

    // Mark keyframes
    if (!is_keyframe) {
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    }

    // Push buffer to appsrc
    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
    if (ret != GST_FLOW_OK) {
        std::cerr << "[WebRTC] Failed to push buffer: " << ret << std::endl;
    }

    frame_count_++;
    if (frame_count_ % 100 == 0) {
        std::cout << "[WebRTC] Pushed " << frame_count_ << " frames" << std::endl;
    }
}

void GstWebRTCPeer::send_data(const std::string& message)
{
    if (!data_channel_) {
        std::cerr << "[WebRTC] Cannot send data - no data channel" << std::endl;
        return;
    }

    gst_webrtc_data_channel_send_string(data_channel_, message.c_str());
    std::cout << "[WebRTC] Sent: " << message << std::endl;
}

// Static callbacks

void GstWebRTCPeer::on_negotiation_needed(GstElement* webrtc, gpointer user_data)
{
    auto* self = static_cast<GstWebRTCPeer*>(user_data);
    std::cout << "[WebRTC] Negotiation needed - creating offer" << std::endl;

    GstPromise* promise = gst_promise_new_with_change_func(
        on_offer_created, self, NULL);
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
}

void GstWebRTCPeer::on_offer_created(GstPromise* promise, gpointer user_data)
{
    auto* self = static_cast<GstWebRTCPeer*>(user_data);

    const GstStructure* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);

    if (!offer) {
        std::cerr << "[WebRTC] Failed to create offer" << std::endl;
        gst_promise_unref(promise);
        return;
    }

    // Set local description
    GstPromise* local_promise = gst_promise_new();
    g_signal_emit_by_name(self->webrtc_, "set-local-description", offer, local_promise);
    gst_promise_interrupt(local_promise);
    gst_promise_unref(local_promise);

    // Get SDP text
    gchar* sdp_text = gst_sdp_message_as_text(offer->sdp);
    std::string sdp_str(sdp_text);
    g_free(sdp_text);

    std::cout << "[WebRTC] Created offer" << std::endl;

    // Notify callback
    if (self->on_local_description_) {
        self->on_local_description_("offer", sdp_str);
    }

    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);
}

void GstWebRTCPeer::on_ice_candidate(GstElement* webrtc, guint mlineindex,
                                      gchar* candidate, gpointer user_data)
{
    auto* self = static_cast<GstWebRTCPeer*>(user_data);
    std::cout << "[WebRTC] ICE candidate: " << candidate << std::endl;

    if (self->on_ice_candidate_) {
        self->on_ice_candidate_(mlineindex, std::string(candidate));
    }
}

void GstWebRTCPeer::on_ice_connection_state_notify(GstElement* webrtc, GParamSpec* pspec,
                                                    gpointer user_data)
{
    auto* self = static_cast<GstWebRTCPeer*>(user_data);

    GstWebRTCICEConnectionState state;
    g_object_get(webrtc, "ice-connection-state", &state, NULL);

    const char* state_str = "unknown";
    bool connected = false;

    switch (state) {
        case GST_WEBRTC_ICE_CONNECTION_STATE_NEW:
            state_str = "new";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:
            state_str = "checking";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:
            state_str = "connected";
            connected = true;
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:
            state_str = "completed";
            connected = true;
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
            state_str = "failed";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED:
            state_str = "disconnected";
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED:
            state_str = "closed";
            break;
    }

    std::cout << "[WebRTC] ICE connection state: " << state_str << std::endl;

    self->connected_.store(connected);

    if (self->on_state_change_) {
        self->on_state_change_(connected);
    }
}

void GstWebRTCPeer::on_ice_gathering_state_notify(GstElement* webrtc, GParamSpec* pspec,
                                                   gpointer user_data)
{
    GstWebRTCICEGatheringState state;
    g_object_get(webrtc, "ice-gathering-state", &state, NULL);

    const char* state_str = "unknown";
    switch (state) {
        case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
            state_str = "new";
            break;
        case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
            state_str = "gathering";
            break;
        case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
            state_str = "complete";
            break;
    }

    std::cout << "[WebRTC] ICE gathering state: " << state_str << std::endl;
}

void GstWebRTCPeer::on_data_channel(GstElement* webrtc, GObject* channel, gpointer user_data)
{
    auto* self = static_cast<GstWebRTCPeer*>(user_data);
    std::cout << "[WebRTC] Incoming data channel" << std::endl;

    // Connect signals for incoming channel
    g_signal_connect(channel, "on-open",
        G_CALLBACK(on_data_channel_open), self);
    g_signal_connect(channel, "on-message-string",
        G_CALLBACK(on_data_channel_message), self);
}

void GstWebRTCPeer::on_data_channel_open(GstWebRTCDataChannel* channel, gpointer user_data)
{
    std::cout << "[WebRTC] Data channel opened" << std::endl;
}

void GstWebRTCPeer::on_data_channel_message(GstWebRTCDataChannel* channel, gchar* message,
                                             gpointer user_data)
{
    auto* self = static_cast<GstWebRTCPeer*>(user_data);
    std::cout << "[WebRTC] Received: " << message << std::endl;

    if (self->on_data_channel_message_) {
        self->on_data_channel_message_(std::string(message));
    }
}
