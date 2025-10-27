#ifndef MEDIA_STREAMER_RTSP_H
#define MEDIA_STREAMER_RTSP_H

#include "media_streamer.h"
#include <memory>
#include <string>

// Forward declaration
class KdRtspServer;

/**
 * MediaStreamerRtsp - RTSP live streaming implementation
 *
 * Streams H.265 encoded video via RTSP using K230 SDK rtsp_server (live555)
 */
class MediaStreamerRtsp : public MediaStreamerAbstract {
public:
    MediaStreamerRtsp(int port = 8554); // RTSP default port
    ~MediaStreamerRtsp() override;

    /**
     * Initialize RTSP server and session
     * @param config Session name (e.g., "live" for rtsp://ip:8554/live)
     * @param width Video width (e.g., 1920)
     * @param height Video height (e.g., 1080)
     * @return 0 on success, negative on error
     */
    int init(const char* config, int width, int height) override;

    /**
     * Send H.265 encoded video frame via RTSP
     * @param data Pointer to encoded H.265 frame data
     * @param data_length Length of encoded data in bytes
     * @param pts_us Presentation timestamp in microseconds
     * @param is_keyframe True if this is an I-frame
     * @return 0 on success, negative on error
     */
    int write_video_frame(const uint8_t* data, size_t data_length,
                         uint64_t pts_us, bool is_keyframe) override;

    // DISABLED: Metadata not supported
    // /**
    //  * Write metadata (NOT SUPPORTED for RTSP)
    //  * RTSP does not support subtitle/metadata tracks in standard implementations
    //  * @return 0 (no-op)
    //  */
    // int write_metadata(const char* metadata_json, uint64_t pts_us) override;

    /**
     * Stop RTSP server and destroy session
     */
    void stop() override;

    /**
     * Check if RTSP server is initialized and ready
     */
    bool is_ready() const override { return rtsp_initialized_; }

private:
    std::unique_ptr<KdRtspServer> rtsp_server_;
    std::string session_name_;
    bool rtsp_initialized_;
    int port_;
};

#endif // MEDIA_STREAMER_RTSP_H