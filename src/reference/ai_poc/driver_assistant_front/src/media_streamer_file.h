#ifndef MEDIA_STREAMER_FILE_H
#define MEDIA_STREAMER_FILE_H

#include "media_streamer.h"

/**
 * MediaStreamerFile - MP4 file recording implementation
 *
 * Records H.265 encoded video to MP4 file using K230 SDK mp4_format API
 */
class MediaStreamerFile : public MediaStreamerAbstract {
public:
    MediaStreamerFile();
    ~MediaStreamerFile() override;

    /**
     * Initialize MP4 file writer
     * @param config Path to output MP4 file (e.g., "/mnt/bb/recording.mp4")
     * @param width Video width (e.g., 1920)
     * @param height Video height (e.g., 1080)
     * @return 0 on success, negative on error
     */
    int init(const char* config, int width, int height) override;

    /**
     * Write H.265 encoded video frame to MP4 file
     * @param data Pointer to encoded H.265 frame data
     * @param data_length Length of encoded data in bytes
     * @param pts_us Presentation timestamp in microseconds
     * @param is_keyframe True if this is an I-frame
     * @return 0 on success, negative on error
     */
    int write_video_frame(const uint8_t* data, size_t data_length,
                         uint64_t pts_us, bool is_keyframe) override;

    // DISABLED: Subtitle track not supported in fragmented MP4
    // /**
    //  * Write metadata as text subtitles in MP4 file
    //  * @param metadata_json JSON string containing metadata
    //  * @param pts_us Presentation timestamp in microseconds
    //  * @return 0 on success, negative on error
    //  */
    // int write_metadata(const char* metadata_json, uint64_t pts_us) override;

    /**
     * Stop recording and close MP4 file
     */
    void stop() override;

    /**
     * Check if MP4 file writer is initialized and ready
     */
    bool is_ready() const override { return mp4_initialized_; }

private:
    // MP4 file writer handles (using K230 SDK mp4_format API)
    void* mp4_muxer_;
    void* video_track_handle_;
    // void* subtitle_track_handle_;  // DISABLED: Not supported in fragmented MP4
    bool mp4_initialized_;
};

#endif // MEDIA_STREAMER_FILE_H