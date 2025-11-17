//
// Created by misha on 17/11/2025.
//

#ifndef K230_SDK_BLACK_BOX_H
#define K230_SDK_BLACK_BOX_H
#include <fstream>
#include <string>
#include <vector>

#include "media_streamer_file.h"
#include "../../driver_assistant_detector/common_ipc.h"


class black_box {
    MediaStreamerFile _streamer;
    uint64_t _streamer_first_pts = 0;

    size_t _streamer_recording_number = 0;
    //size_t _prev_current_recording_number = 0;

    std::ofstream _file_detections;
    // it may have a lower value than _current_recording_number
    size_t _file_detections_number = 0;
    uint64_t _file_detections_first_pts = 0;

public:
    static constexpr size_t max_size = 1024ULL * 1024 * 1024 * 2;  // 2 GB
    static constexpr const char* prefix = "recording_";

    explicit black_box(std::string dir_path, int width, int height);

    std::string dir_path;
    const int width;
    const int height;

    int write_video_frame(const uint8_t* data, size_t data_length, uint64_t pts_us, bool is_keyframe);
    void write_detections(uint64_t pts,
        const std::vector<driver_assistant_detector::DetectionNormalizedCommon> &pre_detections,
        const std::vector<driver_assistant_detector::DetectionNormalizedCommon> &detections,
        const driver_assistant_detector::DetectedSituation &situation);

    void flush_files();

    static std::vector<size_t> list_of_recordings(const char *dir_path);
    static size_t get_free_space(const char *dir_path);
};


#endif //K230_SDK_BLACK_BOX_H