//
// Created by misha on 17/11/2025.
//

#include "black_box.h"

#include <algorithm>
#include <string>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>
#include <charconv>
#include <iostream>
#include <unistd.h>
#include <ext/stdio_filebuf.h>

namespace fs = std::filesystem;

black_box::black_box(std::string dir_path, int width, int height) : dir_path(std::move(dir_path)), width(width), height(height) {
    auto list = list_of_recordings(this->dir_path.c_str());
    auto max = std::max_element(list.cbegin(), list.cend());
    _streamer_recording_number = max != list.cend() ? *max : 0;


}

int black_box::write_video_frame(const uint8_t *data, size_t data_length, uint64_t pts_us, bool is_keyframe) {
    if (!_streamer.is_ready() || (is_keyframe && _streamer.get_total_data_len() > max_size)) {
        _streamer.stop();

        // Construct the recording file path: <dir_path>/recording_<number>.mp4
        std::string recording_path = this->dir_path + "/" + prefix + std::to_string(++_streamer_recording_number) + ".mp4";
        _streamer.init(recording_path.c_str(), width, height);

        _streamer_first_pts = pts_us;
    }

    return _streamer.write_video_frame(data, data_length, pts_us - _streamer_first_pts, is_keyframe);
}

void black_box::write_detections(uint64_t pts,
    const std::vector<driver_assistant_detector::DetectionNormalizedCommon> &pre_detections,
    const std::vector<driver_assistant_detector::DetectionNormalizedCommon> &detections,
    const driver_assistant_detector::DetectedSituation &situation) {
    if (!_streamer.is_ready() || _streamer_first_pts == 0) return;

    // if we have a detection file from the prev session but pts>=_streamer_first_pts, close this file and write data to new
    if (_file_detections.is_open() && _file_detections_number < _streamer_recording_number && pts >= _streamer_first_pts)
        _file_detections.close();

    if (!_file_detections.is_open()) {
        _file_detections_number = _streamer_recording_number;
        _file_detections_first_pts = _streamer_first_pts;

        std::string recording_path = this->dir_path + "/" + prefix + std::to_string(_file_detections_number) + ".txt";
        _file_detections.open(recording_path);
    }

    if (pts < _file_detections_first_pts) return; // because of some bug

    // Write timestamp in milliseconds
    _file_detections << ((pts - _file_detections_first_pts) / 1000) << ";:";

    // Write detected situation color
    _file_detections << driver_assistant_detector::DetectedSituationColor_str[situation.color];
    if (situation.arrow_left)
        _file_detections << " arrow_left";
    if (situation.arrow_right)
        _file_detections << " arrow_right";
    if (situation.arrow_forward)
        _file_detections << " arrow_forward";
    _file_detections << ";:";

    // add pre detections
    for (const auto &it: pre_detections) {
        _file_detections << driver_assistant_detector::detect_classes_str[it.class_id] << " "
                        << std::fixed << std::setprecision(2) << it.confidence << " "
                        << std::setprecision(10) << it.x << " " << it.y << " "
                        << std::defaultfloat << it.w << " " << it.h << ";";
    }
    _file_detections << ";:";

    // add detections
    for (const auto &it: detections) {
        _file_detections << driver_assistant_detector::detect_classes_str[it.class_id] << " "
                        << std::fixed << std::setprecision(2) << it.confidence << " "
                        << std::setprecision(10) << it.x << " " << it.y << " "
                        << std::defaultfloat << it.w << " " << it.h << ";";
    }
    _file_detections << "\n";
}

void black_box::flush_files() {
    if (!_file_detections.is_open()) return;
    _file_detections.flush(); // Flush C++ stream buffer (equivalent to fflush)

    // Get file descriptor from ofstream using GNU extension
    auto *filebuf = dynamic_cast<__gnu_cxx::stdio_filebuf<char> *>(_file_detections.rdbuf());
    if (filebuf) {
        int fd = filebuf->fd();
        fsync(fd); // Sync to disk (equivalent to fsync(fileno(...)))
    }
}

std::vector<size_t> black_box::list_of_recordings(const char *dir_path) {
    std::cout << dir_path << std::endl;
    std::set<size_t> recording_numbers_set;  // Use set for automatic sorting and deduplication

    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        return {};  // Return empty vector if path doesn't exist or isn't a directory
    }

    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) {
            continue;  // Skip non-files (directories, symlinks, etc.)
        }

        std::string filename = entry.path().filename().string();

        // Check if filename starts with "recording_"
        if (filename.rfind(prefix, 0) == 0) {  // rfind at position 0 checks prefix
            // Extract the number part after "recording_"
            std::string num_part = filename.substr(10);  // Skip "recording_"

            // Find the extension (first dot after the number)
            size_t dot_pos = num_part.find('.');
            if (dot_pos != std::string::npos) {
                num_part = num_part.substr(0, dot_pos);  // Get only the number part
            }

            // Parse the number using std::from_chars (C++17)
            size_t num;
            auto [ptr, ec] = std::from_chars(num_part.data(), num_part.data() + num_part.size(), num);

            if (ec == std::errc()) {  // Successfully parsed
                recording_numbers_set.insert(num);
            }
        }
    }

    // Convert set to vector (already sorted)
    return std::vector<size_t>(recording_numbers_set.begin(), recording_numbers_set.end());
}

size_t black_box::get_free_space(const char *dir_path) {
    if (!fs::exists(dir_path)) {
        return 0;  // Path doesn't exist, return 0
    }

    try {
        fs::space_info space = fs::space(dir_path);
        return static_cast<size_t>(space.available);  // Return available space in bytes
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error getting free space for " << dir_path << ": " << e.what() << std::endl;
        return 0;
    }
}
