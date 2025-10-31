#include "detector_post_processing.h"
#include <algorithm>
#include <numeric>

namespace detector_post_processing {
    bool is_traffic_light(const detect_classes_t type) {
        return type == detect_classes_t::TRAFFIC_LIGHT ||
               type == detect_classes_t::TRAFFIC_LIGHT_GREEN ||
               type == detect_classes_t::TRAFFIC_LIGHT_RED ||
               type == detect_classes_t::TRAFFIC_LIGHT_RED_YELLOW ||
               type == detect_classes_t::TRAFFIC_LIGHT_YELLOW;
    }

    bool is_color(const detect_classes_t type) {
        return type == detect_classes_t::COLOR_GREEN ||
               type == detect_classes_t::COLOR_RED;
    }

    std::vector<DetectionNormalized> post_process(std::vector<DetectionNormalized> detections) {
        // Prevent traffic light interference
        for (int i = 0; i < detections.size(); i++) {
            if (!is_traffic_light(detections[i].class_id)) continue;
            for (int j = 0; j < detections.size(); j++) {
                if (i == j || !is_traffic_light(detections[j].class_id)) continue; // Skip comparing with itself

                int item_with_min_area = std::min(i, j, [&detections](const int a, const int b) {
                    return detections[a].box.area() < detections[b].box.area();
                });

                // if 2 traffic lights are intersected
                if ((detections[i].box & detections[j].box).area() >= detections[item_with_min_area].box.area() * 0.5) {
                    int item_with_min_conf = std::min(i, j, [&detections](const int a, const int b) {
                        return detections[a].confidence < detections[b].confidence;
                    });
                    detections.erase(detections.begin() + item_with_min_conf);
                    i = -1; // to check again
                    break;
                }
            }
        }

        // Prevent traffic color interference
        for (int i = 0; i < detections.size(); i++) {
            if (!is_color(detections[i].class_id)) continue;
            for (int j = 0; j < detections.size(); j++) {
                if (i == j || !is_color(detections[j].class_id)) continue; // Skip comparing with itself
                printf("found %d %d\n", i, j);

                int item_with_min_area = std::min(i, j, [&detections](const int a, const int b) {
                    return detections[a].box.area() < detections[b].box.area();
                });

                // if 2 colors are intersected
                if ((detections[i].box & detections[j].box).area() >= detections[item_with_min_area].box.area() * 0.5) {
                    int item_with_min_conf = std::min(i, j, [&detections](const int a, const int b) {
                        return detections[a].confidence < detections[b].confidence;
                    });
                    detections.erase(detections.begin() + item_with_min_conf);
                    i = -1; // to check again
                    break;
                }
            }
        }

        return detections;
    }


} // namespace detector_post_processing