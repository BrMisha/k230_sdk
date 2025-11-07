#pragma once

#include "ob_det.h"
#include "utils.h"
#include <opencv2/opencv.hpp>
#include <vector>


class SAHI {
public:
    // Constructor takes OBDet detector and model input size
    explicit SAHI(
        OBDet* detector,
        cv::Size model_input_size,
        float overlap_ratio = 0.2f,
        float nms_threshold = 0.45f
    );
    
    ~SAHI() = default;
    
    // Main detection method with SAHI
    std::vector<Detection> detect(const cv::Mat& image);
    
    // Configuration methods
    void set_overlap_ratio(float ratio);
    float get_overlap_ratio() const { return overlap_ratio_; }
    
    void set_nms_threshold(float threshold) { nms_threshold_ = threshold; }
    float get_nms_threshold() const { return nms_threshold_; }
    
    // Get slice size 
    cv::Size get_slice_size() const { return model_input_size_; }

private:
    OBDet* detector_;               // Pointer to the OBDet detector
    cv::Size model_input_size_;     // Model input size (e.g., 320x320, 640x640)
    float overlap_ratio_;           // Overlap between slices (0.0 - 0.5)
    float nms_threshold_;           // NMS threshold for merging
    
    // Internal SAHI methods
    std::vector<Detection> merge_detections(
        const std::vector<Detection>& all_detections) const;
};
