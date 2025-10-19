#ifndef MEDIA_STREAMER_H
#define MEDIA_STREAMER_H

#include <stdint.h>
#include <stddef.h>

/**
 * MediaStreamerAbstract - Abstract base class for video streaming
 *
 * Base class for different streaming implementations:
 * - MediaStreamerFile: MP4 file recording
 * - MediaStreamerRTSP: RTSP live streaming (future)
 */
class MediaStreamerAbstract {
public:
    virtual ~MediaStreamerAbstract() = default;

    /**
     * Initialize media streamer
     * @param config Configuration string (file path, RTSP URL, etc.)
     * @param width Video width (e.g., 1920)
     * @param height Video height (e.g., 1080)
     * @return 0 on success, negative on error
     */
    virtual int init(const char* config, int width, int height) = 0;

    /**
     * Write H.265 encoded video frame
     * @param data Pointer to encoded H.265 frame data
     * @param data_length Length of encoded data in bytes
     * @param pts_us Presentation timestamp in microseconds
     * @param is_keyframe True if this is an I-frame
     * @return 0 on success, negative on error
     */
    virtual int write_video_frame(const uint8_t* data, size_t data_length,
                                  uint64_t pts_us, bool is_keyframe) = 0;

    /**
     * Write metadata (detection results, etc.)
     * @param metadata_json JSON string containing metadata
     * @param pts_us Presentation timestamp in microseconds (should match video PTS)
     * @return 0 on success, negative on error
     *
     * Example JSON:
     * {"detections":[{"class":"traffic_light","x":0.5,"y":0.3,"w":0.1,"h":0.15,"conf":0.95}]}
     */
    virtual int write_metadata(const char* metadata_json, uint64_t pts_us) = 0;

    /**
     * Stop streaming and close all outputs
     */
    virtual void stop() = 0;

    /**
     * Check if streamer is initialized and ready
     */
    virtual bool is_ready() const = 0;

protected:
};

#endif // MEDIA_STREAMER_H