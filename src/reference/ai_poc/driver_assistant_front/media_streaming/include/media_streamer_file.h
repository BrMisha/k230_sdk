#ifndef MEDIA_STREAMER_FILE_H
#define MEDIA_STREAMER_FILE_H

#include "media_streamer.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

/**
 * MediaStreamerFile - MP4 file recording implementation with async writing
 *
 * Records H.265 encoded video to MP4 file using K230 SDK mp4_format API.
 * Uses a ring buffer and background writer thread to decouple FIFO reads
 * from slow SD card writes.
 */
class MediaStreamerFile : public MediaStreamerAbstract {
public:
    MediaStreamerFile();
    ~MediaStreamerFile() override;

    /**
     * Initialize MP4 file writer and start background writer thread
     * @param config Path to output MP4 file (e.g., "/mnt/bb/recording.mp4")
     * @param width Video width (e.g., 1920)
     * @param height Video height (e.g., 1080)
     * @return 0 on success, negative on error
     */
    int init(const char* config, int width, int height) override;

    /**
     * Queue H.265 encoded video frame for async writing to MP4 file
     * This is fast (~1ms) - just copies to ring buffer
     * @param data Pointer to encoded H.265 frame data
     * @param data_length Length of encoded data in bytes
     * @param pts_us Presentation timestamp in microseconds
     * @param is_keyframe True if this is an I-frame
     * @return 0 on success, -1 if queue full (frame dropped)
     */
    int write_video_frame(const uint8_t* data, size_t data_length,
                         uint64_t pts_us, bool is_keyframe) override;

    /**
     * Stop recording - drains queue, stops writer thread, closes MP4 file
     */
    void stop() override;

    /**
     * Check if MP4 file writer is initialized and ready
     */
    bool is_ready() const override { return mp4_initialized_; }

    /**
     * Get total bytes written to MP4 file (raw encoded data, not including container overhead)
     */
    size_t get_total_data_len() const { return total_data_len_; }

    /**
     * Get current number of frames queued for writing
     */
    size_t queue_size() const { return write_idx_.load() - read_idx_.load(); }

private:
    // Async write ring buffer configuration
    static constexpr size_t NUM_SLOTS = 90;              // 3 sec buffer at 30fps
    static constexpr size_t MAX_FRAME_SIZE = 256 * 1024; // 256KB max per frame

    struct FrameSlot {
        uint8_t data[MAX_FRAME_SIZE];
        size_t len = 0;
        uint64_t pts = 0;
        bool is_keyframe = false;
        std::atomic<bool> ready{false};
    };

    FrameSlot slots_[NUM_SLOTS];
    std::atomic<size_t> write_idx_{0};
    std::atomic<size_t> read_idx_{0};
    std::mutex cv_mutex_;
    std::condition_variable cv_;
    std::thread writer_thread_;
    std::atomic<bool> running_{false};

    void writer_loop();
    int write_frame_sync(const uint8_t* data, size_t data_length,
                        uint64_t pts_us, bool is_keyframe);

    // MP4 file writer handles (using K230 SDK mp4_format API)
    void* mp4_muxer_;
    void* video_track_handle_;
    bool mp4_initialized_;
    std::atomic<size_t> total_data_len_{0};
};

#endif // MEDIA_STREAMER_FILE_H