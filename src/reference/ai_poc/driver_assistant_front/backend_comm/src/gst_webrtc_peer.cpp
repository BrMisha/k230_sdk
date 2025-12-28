#include "gst_webrtc_peer.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <condition_variable>

// Helper struct for dispatching operations to GLib thread
struct GLibDispatchData {
    std::function<void()> func;
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
};

static gboolean glib_dispatch_callback(gpointer user_data) {
    auto* data = static_cast<GLibDispatchData*>(user_data);
    data->func();
    {
        std::lock_guard<std::mutex> lock(data->mtx);
        data->done = true;
    }
    data->cv.notify_one();
    return G_SOURCE_REMOVE;
}

void GstWebRTCPeer::invoke_on_glib_thread(std::function<void()> func)
{
    // If called from the GLib thread, just run directly
    if (g_main_context_is_owner(main_context_)) {
        func();
        return;
    }

    GLibDispatchData data;
    data.func = std::move(func);

    GSource* source = g_idle_source_new();
    g_source_set_callback(source, glib_dispatch_callback, &data, nullptr);
    g_source_attach(source, main_context_);
    g_source_unref(source);

    // Wait for completion
    std::unique_lock<std::mutex> lock(data.mtx);
    data.cv.wait(lock, [&data] { return data.done; });
}

// Helper struct for signaling loop readiness
struct LoopReadyData {
    std::mutex mtx;
    std::condition_variable cv;
    bool ready = false;
};

static gboolean loop_ready_callback(gpointer user_data) {
    auto* data = static_cast<LoopReadyData*>(user_data);
    {
        std::lock_guard<std::mutex> lock(data->mtx);
        data->ready = true;
    }
    data->cv.notify_one();
    return G_SOURCE_REMOVE;
}

GstWebRTCPeer::GstWebRTCPeer(const std::vector<IceServer>& ice_servers)
    : ice_servers_(ice_servers)
{
    // Create GLib main context and loop for proper GStreamer threading
    main_context_ = g_main_context_new();
    main_loop_ = g_main_loop_new(main_context_, FALSE);

    // Synchronization for waiting until main loop is running
    LoopReadyData ready_data;

    // Start GLib main loop in dedicated thread
    gst_thread_ = std::thread([this, &ready_data]() {
        g_main_context_push_thread_default(main_context_);

        // Add idle source to signal when loop is running
        GSource* ready_source = g_idle_source_new();
        g_source_set_callback(ready_source, loop_ready_callback, &ready_data, nullptr);
        g_source_attach(ready_source, main_context_);
        g_source_unref(ready_source);

        std::cout << "[WebRTC] GLib main loop starting..." << std::endl;
        g_main_loop_run(main_loop_);
        std::cout << "[WebRTC] GLib main loop stopped" << std::endl;
        g_main_context_pop_thread_default(main_context_);
    });

    // Wait for the main loop to actually start running
    {
        std::unique_lock<std::mutex> lock(ready_data.mtx);
        ready_data.cv.wait(lock, [&ready_data] { return ready_data.ready; });
    }

    // Now dispatch setup_pipeline() to the GLib thread
    // This ensures all GStreamer elements are created in the correct thread context
    invoke_on_glib_thread([this]() {
        setup_pipeline();
    });
}

GstWebRTCPeer::~GstWebRTCPeer()
{
    std::cout << "[WebRTC] Destructor starting..." << std::endl;

    // Set shutdown flag first to stop callbacks from running
    shutting_down_.store(true);

    // 1. Disconnect signals ON THE GLIB THREAD to ensure no callbacks are in-flight
    //    This is critical: signals must be disconnected from the same thread context
    if (main_loop_ && g_main_loop_is_running(main_loop_)) {
        invoke_on_glib_thread([this]() {
            // Disconnect data channel signals first
            if (data_channel_) {
                g_signal_handlers_disconnect_by_data(G_OBJECT(data_channel_), this);
            }
            if (webrtc_) {
                g_signal_handlers_disconnect_by_data(webrtc_, this);
            }
        });
    }

    // 2. Clear all callbacks under lock to prevent any late invocations
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        on_local_description_ = nullptr;
        on_ice_candidate_ = nullptr;
        on_state_change_ = nullptr;
        on_data_channel_message_ = nullptr;
    }

    // 3. Stop pipeline and clean up GStreamer objects
    {
        std::lock_guard<std::recursive_mutex> lock(gst_mutex_);

        // Unref data channel (was ref'd in on_data_channel or create_offer)
        if (data_channel_) {
            g_object_unref(data_channel_);
            data_channel_ = nullptr;
            std::cout << "[WebRTC] Data channel unreferenced" << std::endl;
        }

        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            std::cout << "[WebRTC] Pipeline destroyed" << std::endl;
        }

        // Clear element pointers (they're owned by pipeline, already freed)
        webrtc_ = nullptr;
    }

    // 4. Stop GLib main loop and wait for thread
    if (main_loop_) {
        g_main_loop_quit(main_loop_);
    }
    if (gst_thread_.joinable()) {
        gst_thread_.join();
    }
    if (main_loop_) {
        g_main_loop_unref(main_loop_);
        main_loop_ = nullptr;
    }
    if (main_context_) {
        g_main_context_unref(main_context_);
        main_context_ = nullptr;
    }

    std::cout << "[WebRTC] Destructor complete" << std::endl;
}

void GstWebRTCPeer::setup_pipeline()
{
    std::cout << "[WebRTC] setup_pipeline() starting..." << std::endl;
    std::cout.flush();

    // Create pipeline with just webrtcbin (data channel only, no video RTP)
    pipeline_ = gst_pipeline_new("webrtc-pipeline");
    if (!pipeline_) {
        std::cerr << "[WebRTC] Failed to create pipeline" << std::endl;
        return;
    }

    webrtc_ = gst_element_factory_make("webrtcbin", "webrtc");
    if (!webrtc_) {
        std::cerr << "[WebRTC] Failed to create webrtcbin" << std::endl;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return;
    }
    std::cout << "[WebRTC] webrtcbin created successfully" << std::endl;

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
                    // Handle both turn:// and turn: formats
                    size_t host_start = (url.find("turn://") == 0) ? 7 : 5;
                    turn_url = "turn://" + server.username + ":" +
                               server.credential + "@" + url.substr(host_start);
                }
                g_object_set(G_OBJECT(webrtc_), "turn-server", turn_url.c_str(), NULL);
                std::cout << "[WebRTC] Set TURN server: " << url << std::endl;
            }
        }
    }

    // Add webrtcbin to pipeline
    gst_bin_add(GST_BIN(pipeline_), webrtc_);

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
    if (shutting_down_.load()) {
        std::cerr << "[WebRTC] Cannot create offer - shutting down" << std::endl;
        return;
    }

    // Dispatch to GLib thread to ensure thread safety
    invoke_on_glib_thread([this]() {
        if (!webrtc_ || shutting_down_.load()) {
            std::cerr << "[WebRTC] Cannot create offer - webrtcbin not initialized or shutting down" << std::endl;
            return;
        }

        // Prevent duplicate offers (set flag early to prevent on_negotiation_needed race)
        if (offer_sent_.exchange(true)) {
            std::cout << "[WebRTC] Offer already sent, skipping" << std::endl;
            return;
        }

        std::cout << "[WebRTC] create_offer() called" << std::endl;
        std::cout.flush();

        // Create data channel (must be before offer for it to be included in SDP)
        if (!data_channel_) {
            GstStructure* options = gst_structure_new("options",
                "ordered", G_TYPE_BOOLEAN, TRUE,
                NULL);
            g_signal_emit_by_name(webrtc_, "create-data-channel", "data", options, &data_channel_);
            gst_structure_free(options);

            if (data_channel_) {
                std::cout << "[WebRTC] Created outgoing data channel" << std::endl;
                g_signal_connect(data_channel_, "on-open",
                    G_CALLBACK(on_data_channel_open), this);
                g_signal_connect(data_channel_, "on-message-string",
                    G_CALLBACK(on_data_channel_message), this);
            } else {
                std::cerr << "[WebRTC] Failed to create data channel" << std::endl;
            }
        }

        // Set pipeline to PLAYING
        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        std::cout << "[WebRTC] set_state(PLAYING) returned " << ret << std::endl;
        std::cout.flush();

        if (ret == GST_STATE_CHANGE_FAILURE) {
            std::cerr << "[WebRTC] Failed to set pipeline to PLAYING" << std::endl;
            return;
        }

        // Create offer synchronously
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

        std::cout << "[WebRTC] Created offer with data channel" << std::endl;
        std::cout.flush();

        // Notify callback
        if (on_local_description_) {
            on_local_description_("offer", sdp_str);
        }

        gst_webrtc_session_description_free(offer);
        gst_promise_unref(promise);
    });
}

void GstWebRTCPeer::set_remote_description(const std::string& type, const std::string& sdp)
{
    if (shutting_down_.load()) {
        std::cerr << "[WebRTC] Cannot set remote description - shutting down" << std::endl;
        return;
    }

    // Dispatch to GLib thread to ensure thread safety
    invoke_on_glib_thread([this, type, sdp]() {
        if (!webrtc_ || shutting_down_.load()) {
            std::cerr << "[WebRTC] Cannot set remote description - webrtcbin not initialized or shutting down" << std::endl;
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

        // Detect rejected m-lines (port=0) to avoid crashes when adding ICE candidates
        rejected_mlines_.clear();
        guint num_medias = gst_sdp_message_medias_len(sdp_msg);
        for (guint i = 0; i < num_medias; i++) {
            const GstSDPMedia* media = gst_sdp_message_get_media(sdp_msg, i);
            if (gst_sdp_media_get_port(media) == 0) {
                rejected_mlines_.insert(i);
                std::cout << "[WebRTC] Media m-line " << i << " rejected (port=0)" << std::endl;
            }
        }

        std::cout << "[WebRTC] Creating session description..." << std::endl;
        std::cout.flush();

        GstWebRTCSDPType sdp_type = (type == "answer") ?
            GST_WEBRTC_SDP_TYPE_ANSWER : GST_WEBRTC_SDP_TYPE_OFFER;

        GstWebRTCSessionDescription* desc =
            gst_webrtc_session_description_new(sdp_type, sdp_msg);

        std::cout << "[WebRTC] Emitting set-remote-description..." << std::endl;
        std::cout.flush();

        // Set remote description (GStreamer copies the data internally)
        g_signal_emit_by_name(webrtc_, "set-remote-description", desc, NULL);

        // Free the description - GStreamer has copied what it needs
        gst_webrtc_session_description_free(desc);

        std::cout << "[WebRTC] Set remote " << type << " - done" << std::endl;
        std::cout.flush();
    });
}

void GstWebRTCPeer::add_ice_candidate(guint mlineindex, const std::string& candidate)
{
    std::cout << "[WebRTC] add_ice_candidate called: mline=" << mlineindex << std::endl;
    std::cout.flush();

    if (shutting_down_.load()) {
        std::cout << "[WebRTC] Skipping - shutting down" << std::endl;
        return;
    }

    // Skip TCP candidates (may have issues with libnice)
    if (candidate.find(" tcp ") != std::string::npos) {
        std::cout << "[WebRTC] Skipping TCP candidate" << std::endl;
        return;
    }

    // Skip candidates for rejected m-lines (port=0 in SDP) - prevents crash
    if (rejected_mlines_.count(mlineindex)) {
        std::cout << "[WebRTC] Skipping candidate for rejected m-line " << mlineindex << std::endl;
        return;
    }

    std::cout << "[WebRTC] Dispatching to GLib thread..." << std::endl;
    std::cout.flush();

    // Dispatch to GLib thread to ensure thread safety
    invoke_on_glib_thread([this, mlineindex, candidate]() {
        std::cout << "[WebRTC] In GLib thread, adding ICE candidate" << std::endl;
        std::cout.flush();

        if (!webrtc_ || shutting_down_.load()) {
            std::cout << "[WebRTC] Skipping - webrtc null or shutting down" << std::endl;
            return;
        }

        g_signal_emit_by_name(webrtc_, "add-ice-candidate", mlineindex, candidate.c_str());
        std::cout << "[WebRTC] g_signal_emit_by_name returned" << std::endl;
        std::cout.flush();
    });

    std::cout << "[WebRTC] add_ice_candidate returning" << std::endl;
    std::cout.flush();
}

void GstWebRTCPeer::send_video_frame(const uint8_t* data, size_t size,
                                       uint64_t pts_us, uint8_t type)
{
    if (!data_channel_ || shutting_down_.load()) {
        return;
    }

    // Flow control: check buffered amount to prevent unbounded queue growth
    // If too much data is queued, skip this frame to prevent memory leak
    guint64 buffered = 0;
    g_object_get(data_channel_, "buffered-amount", &buffered, nullptr);

    // Max 2MB buffered (about 1 second of video at 60KB/frame @ 30fps)
    constexpr guint64 MAX_BUFFERED = 2 * 1024 * 1024;
    if (buffered > MAX_BUFFERED) {
        // Log occasionally to avoid spam
        if (frame_count_ % 30 == 0) {
            std::cerr << "[WebRTC] Dropping frame - buffer full (" << buffered << " bytes)" << std::endl;
        }
        frame_count_++;
        return;
    }

    // Chunking protocol for WebRTC data channel (16KB limit)
    // Chunk format: [4B frame_id][2B chunk_idx][2B total_chunks][payload]
    // First chunk payload: [8B pts][4B len][1B type][video_data...]
    // Subsequent chunks: [video_data...]

    const size_t CHUNK_HEADER_SIZE = 8;      // frame_id(4) + chunk_idx(2) + total_chunks(2)
    const size_t FRAME_HEADER_SIZE = 13;     // pts(8) + len(4) + type(1)
    const size_t MAX_CHUNK_SIZE = 15000;     // Safe limit below 16KB
    const size_t MAX_PAYLOAD = MAX_CHUNK_SIZE - CHUNK_HEADER_SIZE;

    // Total payload = frame header + video data
    const size_t total_payload = FRAME_HEADER_SIZE + size;

    // Calculate number of chunks needed
    uint16_t total_chunks = static_cast<uint16_t>((total_payload + MAX_PAYLOAD - 1) / MAX_PAYLOAD);
    uint32_t frame_id = static_cast<uint32_t>(frame_count_);

    // Build frame header once
    uint8_t frame_header[FRAME_HEADER_SIZE];
    memcpy(frame_header, &pts_us, 8);          // PTS (8 bytes, little-endian)
    uint32_t data_len = static_cast<uint32_t>(size);
    memcpy(frame_header + 8, &data_len, 4);    // Data length (4 bytes)
    frame_header[12] = type;                    // Type (1 byte)

    size_t payload_offset = 0;  // Offset into logical payload (frame_header + video_data)

    for (uint16_t chunk_idx = 0; chunk_idx < total_chunks; chunk_idx++) {
        size_t remaining = total_payload - payload_offset;
        size_t chunk_payload_size = (remaining > MAX_PAYLOAD) ? MAX_PAYLOAD : remaining;
        size_t chunk_size = CHUNK_HEADER_SIZE + chunk_payload_size;

        std::vector<uint8_t> chunk(chunk_size);
        uint8_t* ptr = chunk.data();

        // Chunk header
        memcpy(ptr, &frame_id, 4);
        ptr += 4;
        memcpy(ptr, &chunk_idx, 2);
        ptr += 2;
        memcpy(ptr, &total_chunks, 2);
        ptr += 2;

        // Chunk payload (may span frame_header and video_data)
        size_t written = 0;
        while (written < chunk_payload_size) {
            if (payload_offset < FRAME_HEADER_SIZE) {
                // Copy from frame header
                size_t from_header = std::min(FRAME_HEADER_SIZE - payload_offset,
                                               chunk_payload_size - written);
                memcpy(ptr, frame_header + payload_offset, from_header);
                ptr += from_header;
                payload_offset += from_header;
                written += from_header;
            } else {
                // Copy from video data
                size_t video_offset = payload_offset - FRAME_HEADER_SIZE;
                size_t from_video = chunk_payload_size - written;
                memcpy(ptr, data + video_offset, from_video);
                payload_offset += from_video;
                written += from_video;
            }
        }

        // Send chunk
        GBytes* bytes = g_bytes_new(chunk.data(), chunk_size);
        gst_webrtc_data_channel_send_data(data_channel_, bytes);
        g_bytes_unref(bytes);
    }

    frame_count_++;
    if (frame_count_ % 300 == 0) {  // Log every 10 seconds at 30fps
        std::cout << "[WebRTC] Sent " << frame_count_ << " frames (" << total_chunks
                  << " chunks/frame) via data channel" << std::endl;
    }
}

void GstWebRTCPeer::send_binary(const uint8_t* data, size_t size)
{
    if (!data_channel_ || shutting_down_.load()) {
        std::cerr << "[WebRTC] Cannot send binary - no data channel" << std::endl;
        return;
    }

    GBytes* bytes = g_bytes_new(data, size);
    gst_webrtc_data_channel_send_data(data_channel_, bytes);
    g_bytes_unref(bytes);
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

    // Check if we're shutting down (early check - detailed check in callback)
    if (self->shutting_down_.load()) {
        return;
    }

    // Prevent duplicate offers - create_offer() already handles this
    if (self->offer_sent_.load()) {
        std::cout << "[WebRTC] Negotiation needed but offer already sent, ignoring" << std::endl;
        return;
    }

    std::cout << "[WebRTC] Negotiation needed - creating offer" << std::endl;

    GstPromise* promise = gst_promise_new_with_change_func(
        on_offer_created, self, NULL);
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
}

void GstWebRTCPeer::on_offer_created(GstPromise* promise, gpointer user_data)
{
    auto* self = static_cast<GstWebRTCPeer*>(user_data);

    // Check if we're shutting down
    if (self->shutting_down_.load()) {
        gst_promise_unref(promise);
        return;
    }

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

    // Get callback under lock, checking shutdown state atomically
    OnLocalDescription callback;
    {
        std::lock_guard<std::mutex> lock(self->callback_mutex_);
        if (self->shutting_down_.load() || !self->on_local_description_) {
            gst_webrtc_session_description_free(offer);
            gst_promise_unref(promise);
            return;
        }
        callback = self->on_local_description_;
    }

    callback("offer", sdp_str);

    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);
}

void GstWebRTCPeer::on_ice_candidate(GstElement* webrtc, guint mlineindex,
                                      gchar* candidate, gpointer user_data)
{
    // Copy candidate string immediately (GStreamer owns original memory)
    std::string candidate_str(candidate ? candidate : "");

    auto* self = static_cast<GstWebRTCPeer*>(user_data);

    // Filter outgoing TCP candidates (may have issues)
    if (candidate_str.find(" TCP ") != std::string::npos ||
        candidate_str.find(" tcp ") != std::string::npos) {
        std::cout << "[WebRTC] Skipping outgoing TCP candidate" << std::endl;
        return;
    }

    std::cout << "[WebRTC] ICE candidate: " << candidate_str << std::endl;
    std::cout.flush();

    // Get callback under lock, checking shutdown state atomically
    OnIceCandidate callback;
    {
        std::lock_guard<std::mutex> lock(self->callback_mutex_);
        if (self->shutting_down_.load() || !self->on_ice_candidate_) {
            return;
        }
        callback = self->on_ice_candidate_;
    }

    std::cout << "[WebRTC] Invoking ICE callback..." << std::endl;
    std::cout.flush();
    callback(mlineindex, candidate_str);
    std::cout << "[WebRTC] ICE callback returned" << std::endl;
    std::cout.flush();
}

void GstWebRTCPeer::on_ice_connection_state_notify(GstElement* webrtc, GParamSpec* pspec,
                                                    gpointer user_data)
{
    auto* self = static_cast<GstWebRTCPeer*>(user_data);

    GstWebRTCICEConnectionState state;
    g_object_get(webrtc, "ice-connection-state", &state, NULL);

    const char* state_str = "unknown";
    bool should_notify = false;
    bool connected = false;

    switch (state) {
        case GST_WEBRTC_ICE_CONNECTION_STATE_NEW:
            state_str = "new";
            // Don't notify - initial state
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:
            state_str = "checking";
            // Don't notify - transitional state, connection in progress
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:
            state_str = "connected";
            connected = true;
            should_notify = true;
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:
            state_str = "completed";
            connected = true;
            should_notify = true;
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
            state_str = "failed";
            should_notify = true;  // Notify about failure
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED:
            state_str = "disconnected";
            should_notify = true;  // Notify about disconnection
            break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED:
            state_str = "closed";
            should_notify = true;  // Notify about close
            break;
    }

    std::cout << "[WebRTC] ICE connection state: " << state_str << std::endl;

    self->connected_.store(connected);

    // Only notify on meaningful state changes (connected/disconnected/failed/closed)
    if (should_notify) {
        // Get callback under lock, checking shutdown state atomically
        OnStateChange callback;
        {
            std::lock_guard<std::mutex> lock(self->callback_mutex_);
            if (self->shutting_down_.load() || !self->on_state_change_) {
                return;
            }
            callback = self->on_state_change_;
        }

        callback(connected);
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

    // Check if we're shutting down (early check)
    if (self->shutting_down_.load()) {
        return;
    }

    std::cout << "[WebRTC] Incoming data channel" << std::endl;

    // Store the channel so send_data() works
    self->data_channel_ = GST_WEBRTC_DATA_CHANNEL(g_object_ref(channel));

    // Connect signals for incoming channel
    g_signal_connect(channel, "on-open",
        G_CALLBACK(on_data_channel_open), self);
    g_signal_connect(channel, "on-message-string",
        G_CALLBACK(on_data_channel_message), self);
}

void GstWebRTCPeer::on_data_channel_open(GstWebRTCDataChannel* channel, gpointer user_data)
{
    auto* self = static_cast<GstWebRTCPeer*>(user_data);
    std::cout << "[WebRTC] Data channel opened" << std::endl;

    // Data channel opening means connection is established - set connected flag
    // This is more reliable than waiting for ICE state callback
    self->connected_.store(true);
}

void GstWebRTCPeer::on_data_channel_message(GstWebRTCDataChannel* channel, gchar* message,
                                             gpointer user_data)
{
    auto* self = static_cast<GstWebRTCPeer*>(user_data);

    std::cout << "[WebRTC] Received: " << message << std::endl;

    // Get callback under lock, checking shutdown state atomically
    OnDataChannelMessage callback;
    {
        std::lock_guard<std::mutex> lock(self->callback_mutex_);
        if (self->shutting_down_.load() || !self->on_data_channel_message_) {
            return;
        }
        callback = self->on_data_channel_message_;
    }

    callback(std::string(message));
}
