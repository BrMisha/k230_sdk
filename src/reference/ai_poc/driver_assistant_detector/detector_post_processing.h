#pragma once

#include <vector>
#include <opencv2/opencv.hpp>
#include "utils.h"

namespace detector_post_processing {

    std::vector<DetectionNormalized> post_process(std::vector<DetectionNormalized> detections);

} // namespace detector_post_processing