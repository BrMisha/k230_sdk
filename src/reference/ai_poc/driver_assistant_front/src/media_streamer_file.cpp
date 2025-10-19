#include "media_streamer_file.h"
#include <stdio.h>
#include <string.h>

// K230 SDK headers
#include "k_type.h"
#include "mp4_format.h"

MediaStreamerFile::MediaStreamerFile()
    : mp4_muxer_(nullptr)
    , video_track_handle_(nullptr)
    , mp4_initialized_(false)
    , first_frame_time_stamp_(UINT64_MAX)
{
}

MediaStreamerFile::~MediaStreamerFile() {
    stop();
}

int MediaStreamerFile::init(const char* config, int width, int height) {
    k_s32 ret;

    // =========================================================================
    // Initialize MP4 file writer
    // =========================================================================
    k_mp4_config_s mp4_config;
    memset(&mp4_config, 0, sizeof(mp4_config));
    mp4_config.config_type = K_MP4_CONFIG_MUXER;
    strncpy(mp4_config.muxer_config.file_name, config,
            sizeof(mp4_config.muxer_config.file_name) - 1);
    mp4_config.muxer_config.fmp4_flag = 0; // Standard MP4 for file recording

    ret = kd_mp4_create((KD_HANDLE*)&mp4_muxer_, &mp4_config);
    if (ret != 0) {
        printf("MediaStreamerFile: kd_mp4_create failed: %d\n", ret);
        return ret;
    }

    // Create H.265 video track
    k_mp4_track_info_s video_track_info;
    memset(&video_track_info, 0, sizeof(video_track_info));
    video_track_info.track_type = K_MP4_STREAM_VIDEO;
    video_track_info.time_scale = 1000; // Timescale in milliseconds
    video_track_info.video_info.width = width;
    video_track_info.video_info.height = height;
    video_track_info.video_info.codec_id = K_MP4_CODEC_ID_H265;

    ret = kd_mp4_create_track((KD_HANDLE)mp4_muxer_,
                              (KD_HANDLE*)&video_track_handle_,
                              &video_track_info);
    if (ret != 0) {
        printf("MediaStreamerFile: kd_mp4_create_track (video) failed: %d\n", ret);
        kd_mp4_destroy((KD_HANDLE)mp4_muxer_);
        mp4_muxer_ = nullptr;
        return ret;
    }

    mp4_initialized_ = true;
    printf("MediaStreamerFile: MP4 initialized - %s (%dx%d)\n",
           config, width, height);

    return 0;
}

int MediaStreamerFile::write_video_frame(const uint8_t* data, size_t data_length,
                                         uint64_t pts_us, bool is_keyframe) {
    if (!mp4_initialized_) {
        printf("MediaStreamerFile: Not initialized\n");
        return -1;
    }

    if (first_frame_time_stamp_ == UINT64_MAX)
        first_frame_time_stamp_ = pts_us;

    pts_us -= first_frame_time_stamp_;

    // Write to MP4 file
    k_mp4_frame_data_s frame_data;
    memset(&frame_data, 0, sizeof(frame_data));
    frame_data.codec_id = K_MP4_CODEC_ID_H265;
    frame_data.data = const_cast<uint8_t*>(data);
    frame_data.data_length = data_length;
    frame_data.time_stamp = pts_us;

    k_s32 ret = kd_mp4_write_frame((KD_HANDLE)mp4_muxer_,
                                   (KD_HANDLE)video_track_handle_,
                                   &frame_data);
    if (ret != 0) {
        printf("MediaStreamerFile: kd_mp4_write_frame failed: %d\n", ret);
        return ret;
    }

    return 0;
}

int MediaStreamerFile::write_metadata(const char* metadata_json, uint64_t pts_us) {
    // Metadata track not supported by K230 SDK mp4_format
    // Can be logged to a separate file if needed in the future
    (void)metadata_json;
    (void)pts_us;
    return 0;
}

void MediaStreamerFile::stop() {
    // Close MP4 file
    if (mp4_initialized_) {
        kd_mp4_destroy_tracks((KD_HANDLE)mp4_muxer_);
        kd_mp4_destroy((KD_HANDLE)mp4_muxer_);
        mp4_muxer_ = nullptr;
        video_track_handle_ = nullptr;
        mp4_initialized_ = false;
        printf("MediaStreamerFile: MP4 closed\n");
    }
}