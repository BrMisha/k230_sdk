#include "media_streamer_rtsp.h"
#include <stdio.h>
#include "rtsp_server.h"

MediaStreamerRtsp::MediaStreamerRtsp(int port)
    : rtsp_server_(nullptr)
    , rtsp_initialized_(false)
    , port_(port)
{
}

MediaStreamerRtsp::~MediaStreamerRtsp() {
    stop();
}

int MediaStreamerRtsp::init(const char* config, int width, int height) {
    if (!config) {
        printf("MediaStreamerRtsp: config (session name) is required\n");
        return -1;
    }

    session_name_ = config;

    // Create RTSP server instance
    rtsp_server_ = std::make_unique<KdRtspServer>();

    // Initialize RTSP server
    int ret = rtsp_server_->Init(port_);
    if (ret != 0) {
        printf("MediaStreamerRtsp: Failed to initialize RTSP server on port %d\n", port_);
        rtsp_server_.reset();
        return ret;
    }

    // Create streaming session with H.265 video
    SessionAttr session_attr;
    session_attr.with_video = true;
    session_attr.video_type = VideoType::kVideoTypeH265;
    session_attr.with_audio = false;
    session_attr.with_audio_backchannel = false;

    ret = rtsp_server_->CreateSession(session_name_, session_attr);
    if (ret != 0) {
        printf("MediaStreamerRtsp: Failed to create session '%s'\n", session_name_.c_str());
        rtsp_server_->DeInit();
        rtsp_server_.reset();
        return ret;
    }

    // Start RTSP server event loop
    rtsp_server_->Start();

    rtsp_initialized_ = true;
    printf("MediaStreamerRtsp: RTSP server started on port %d, session '%s'\n",
           port_, session_name_.c_str());
    printf("MediaStreamerRtsp: To play: ffplay -fflags nobuffer -flags low_delay -framedrop -strict experimental -vf setpts=0 -probesize 32 -analyzeduration 0 rtsp://<ip>:%d/%s\n",
           port_, session_name_.c_str());

    return 0;
}

int MediaStreamerRtsp::write_video_frame(const uint8_t* data, size_t data_length,
                                         uint64_t pts_us, bool is_keyframe) {
    if (!rtsp_initialized_) {
        printf("MediaStreamerRtsp: Not initialized\n");
        return -1;
    }

    // Note: timestamp parameter is currently ignored by KdRtspServer
    // (uses gettimeofday() instead), but pass microseconds anyway
    int ret = rtsp_server_->SendVideoData(session_name_, data, data_length, pts_us);
    if (ret != 0) {
        printf("MediaStreamerRtsp: SendVideoData failed: %d\n", ret);
        return ret;
    }

    return 0;
}

int MediaStreamerRtsp::write_metadata(const char* metadata_json, uint64_t pts_us) {
    // RTSP does not support subtitle/metadata tracks in standard implementations
    // Metadata would require custom RTSP extensions or separate channel
    // For now, this is a no-op
    (void)metadata_json;
    (void)pts_us;
    return 0;
}

void MediaStreamerRtsp::stop() {
    if (rtsp_initialized_) {
        rtsp_server_->Stop();
        rtsp_server_->DestroySession(session_name_);
        rtsp_server_->DeInit();
        rtsp_server_.reset();
        rtsp_initialized_ = false;
        printf("MediaStreamerRtsp: RTSP server stopped\n");
    }
}