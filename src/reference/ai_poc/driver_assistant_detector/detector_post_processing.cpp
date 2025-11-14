#include "detector_post_processing.h"
#include <algorithm>
#include <numeric>
#include <vector>

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
        /*printf("=== Input detections: %zu ===\n", detections.size());
        for (size_t i = 0; i < detections.size(); i++) {
            printf("[%zu] class=%d conf=%.7f box=(%.7f, %.7f, %.7f, %.7f)\n",
                   i, detections[i].class_id, detections[i].confidence,
                   detections[i].box.x, detections[i].box.y,
                   detections[i].box.width, detections[i].box.height);
        }*/

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

        auto find_in_box = [&detections](const cv::Rect2f &box, const detect_classes_t item_to_find) -> vector<int> {
            vector<int> list;

            for (int i = 0; i < detections.size(); i++) {
                auto &detection = detections[i];
                if (detection.class_id != item_to_find) continue;
                // if more than 80% of detection inside a box
                if ((box & detection.box).area() > (detection.box.area() * 0.6f)) list.push_back(i);
            }

            return list;
        };

        for (int i = 0; i < detections.size(); i++) {
            auto &detection = detections[i];

            switch (detection.class_id) {
                case detect_classes_t::TRAFFIC_LIGHT_GREEN: {
                    auto res = find_in_box(detection.box, detect_classes_t::COLOR_GREEN);
                    if (res.size() != 1) break; // only a single color is ok

                    detection.class_id = detect_classes_t::APPROVED_GREEN;
                    detection.confidence += detections[res.front()].confidence;
                    detection.box = detection.box | detections[res.front()].box;

                    detections.erase(detections.begin() + res.front());
                    i = -1; // to check again
                } break;
                case detect_classes_t::TRAFFIC_LIGHT_RED: {
                    auto res = find_in_box(detection.box, detect_classes_t::COLOR_RED);
                    if (res.size() != 1) break; // only a single color is ok

                    detection.class_id = detect_classes_t::APPROVED_RED;
                    detection.confidence += detections[res.front()].confidence;
                    detection.box = detection.box | detections[res.front()].box;

                    detections.erase(detections.begin() + res.front());
                    i = -1; // to check again
                } break;
                case detect_classes_t::TRAFFIC_LIGHT_RED_YELLOW: {
                    auto res = find_in_box(detection.box, detect_classes_t::COLOR_RED);
                    if (res.size() != 2) break;

                    detection.class_id = detect_classes_t::APPROVED_RED_YELLOW;
                    detection.confidence += detections[res.front()].confidence; // just use first
                    detection.box = detection.box | detections[res.at(0)].box | detections[res.at(1)].box;

                    detections.erase(detections.begin() + res.at(1));
                    detections.erase(detections.begin() + res.at(0));
                    i = -1; // to check again
                } break;
                case detect_classes_t::TRAFFIC_LIGHT_YELLOW: {
                    auto res = find_in_box(detection.box, detect_classes_t::COLOR_RED);
                    if (res.size() != 1) break; // only a single color is ok

                    detection.class_id = detect_classes_t::APPROVED_YELLOW;
                    detection.confidence += detections[res.front()].confidence;
                    detection.box = detection.box | detections[res.front()].box;

                    detections.erase(detections.begin() + res.front());
                    i = -1; // to check again
                } break;
                default: ;
            }
        }

        enum CHECK_FOR_OBJECT_ON { LEFT = 0b1, RIGHT = 0b10, TOP = 0b100, BOTTOM = 0b1000};
        auto check_for_object_on = [&detections](const cv::Rect2f &box, int sides, float max_distance,
            float min_confidence, std::initializer_list<detect_classes_t> classes) -> DetectionNormalized* {

            // Iterate through all detections
            for (auto &detection : detections) {
                // Check if detection class is in the requested classes list
                bool class_matches = false;
                for (auto cls : classes) {
                    if (detection.class_id == cls) {
                        class_matches = true;
                        break;
                    }
                }
                if (!class_matches) continue;

                // Check if confidence meets minimum threshold
                if (detection.confidence < min_confidence) continue;

                const auto &obj_box = detection.box;

                // Check proximity on requested sides
                bool is_near = false;

                // First, reject if object extends beyond non-requested sides
                // If RIGHT is not requested, object should not extend beyond box's right edge
                if (!(sides & RIGHT) && obj_box.x > box.x + box.width) {
                    continue; // Object is to the right, skip it
                }
                // If LEFT is not requested, object should not extend beyond box's left edge
                if (!(sides & LEFT) && obj_box.x + obj_box.width < box.x) {
                    continue; // Object is to the left, skip it
                }
                // If BOTTOM is not requested, object should not extend beyond box's bottom edge
                if (!(sides & BOTTOM) && obj_box.y > box.y + box.height) {
                    continue; // Object is below, skip it
                }
                // If TOP is not requested, object should not extend beyond box's top edge
                if (!(sides & TOP) && obj_box.y + obj_box.height < box.y) {
                    continue; // Object is above, skip it
                }

                // Now check if object is near/overlapping on requested sides
                // Check LEFT side: object is to the left OR overlaps from left
                if (sides & LEFT) {
                    // Object's right edge is at or before box's left edge (with max_distance tolerance)
                    bool near_left = (obj_box.x + obj_box.width <= box.x + max_distance) &&
                                     (obj_box.y < box.y + box.height) &&
                                     (obj_box.y + obj_box.height > box.y);
                    if (near_left) is_near = true;
                }

                // Check RIGHT side: object is to the right OR overlaps from right
                if (sides & RIGHT) {
                    // Object's left edge is at or after box's right edge (with max_distance tolerance)
                    bool near_right = (obj_box.x >= box.x + box.width - max_distance) &&
                                      (obj_box.y < box.y + box.height) &&
                                      (obj_box.y + obj_box.height > box.y);
                    if (near_right) is_near = true;
                }

                // Check TOP side: object is above OR overlaps from top
                if (sides & TOP) {
                    // Object's bottom edge is at or before box's top edge (with max_distance tolerance)
                    bool near_top = (obj_box.y + obj_box.height <= box.y + max_distance) &&
                                    (obj_box.x < box.x + box.width) &&
                                    (obj_box.x + obj_box.width > box.x);
                    if (near_top) is_near = true;
                }

                // Check BOTTOM side: object is below OR overlaps from bottom
                if (sides & BOTTOM) {
                    // Object's top edge is at or after box's bottom edge (with max_distance tolerance)
                    bool near_bottom = (obj_box.y >= box.y + box.height - max_distance) &&
                                       (obj_box.x < box.x + box.width) &&
                                       (obj_box.x + obj_box.width > box.x);
                    if (near_bottom) is_near = true;
                }

                // If object is near on any of the requested sides, return it
                if (is_near) {
                    return &detection;
                }
            }

            return nullptr;
        };

        // process arrow right
        for (int i = 0; i < detections.size(); i++) {
            auto &detection = detections[i];
            if (detection.confidence >= 1.0f) continue;

            DetectionNormalized *obj = nullptr;
            switch (detection.class_id) {
                case detect_classes_t::ARROW_RIGHT:
                    obj = check_for_object_on(detection.box, LEFT|TOP|BOTTOM, detection.box.width, 0.5, {
                        detect_classes_t::TRAFFIC_LIGHT,
                        detect_classes_t::TRAFFIC_LIGHT_GREEN, detect_classes_t::TRAFFIC_LIGHT_RED,
                        detect_classes_t::TRAFFIC_LIGHT_RED_YELLOW, detect_classes_t::TRAFFIC_LIGHT_YELLOW,
                        detect_classes_t::APPROVED_GREEN, detect_classes_t::APPROVED_RED,
                        detect_classes_t::APPROVED_YELLOW, detect_classes_t::APPROVED_RED_YELLOW
                    });
                case detect_classes_t::TL_ARROW_LEFT:
                    obj = check_for_object_on(detection.box, RIGHT|TOP|BOTTOM, detection.box.width, 0.5, {
                        detect_classes_t::TRAFFIC_LIGHT,
                        detect_classes_t::TRAFFIC_LIGHT_GREEN, detect_classes_t::TRAFFIC_LIGHT_RED,
                        detect_classes_t::TRAFFIC_LIGHT_RED_YELLOW, detect_classes_t::TRAFFIC_LIGHT_YELLOW,
                        detect_classes_t::APPROVED_GREEN, detect_classes_t::APPROVED_RED,
                        detect_classes_t::APPROVED_YELLOW, detect_classes_t::APPROVED_RED_YELLOW
                    });
                case detect_classes_t::TL_ARROW_FORWARD: {
                    obj = check_for_object_on(detection.box, RIGHT|LEFT|TOP|BOTTOM, detection.box.width, 0.5, {
                        detect_classes_t::TRAFFIC_LIGHT,
                        detect_classes_t::TRAFFIC_LIGHT_GREEN, detect_classes_t::TRAFFIC_LIGHT_RED,
                        detect_classes_t::TRAFFIC_LIGHT_RED_YELLOW, detect_classes_t::TRAFFIC_LIGHT_YELLOW,
                        detect_classes_t::APPROVED_GREEN, detect_classes_t::APPROVED_RED,
                        detect_classes_t::APPROVED_YELLOW, detect_classes_t::APPROVED_RED_YELLOW
                    });
                } break;

                default: break;
            }

            if (obj)
                detection.confidence = 1.0;
        }

        return detections;
    }

    DetectedSituation define_situation(const std::vector<DetectionNormalized> &detections) {
        DetectedSituation situation;

        // Collect indices of items matching filter condition
        std::vector<size_t> filtered_indices;
        for (size_t i = 0; i < detections.size(); i++) {
            switch (detections[i].class_id) {
                case detect_classes_t::TRAFFIC_LIGHT_GREEN:
                case detect_classes_t::TRAFFIC_LIGHT_RED:
                case detect_classes_t::TRAFFIC_LIGHT_RED_YELLOW:
                case detect_classes_t::TRAFFIC_LIGHT_YELLOW:
                case detect_classes_t::COLOR_GREEN:
                case detect_classes_t::COLOR_RED:
                case detect_classes_t::APPROVED_GREEN:
                case detect_classes_t::APPROVED_RED:
                case detect_classes_t::APPROVED_YELLOW:
                case detect_classes_t::APPROVED_RED_YELLOW:
                    if (detections[i].confidence > 0.85)
                        filtered_indices.push_back(i);
                    break;
                default:
                    break;
            }
        }

        if (filtered_indices.empty()) return situation;

        auto min_idx_it = std::min_element(filtered_indices.begin(), filtered_indices.end(),
                                           [&detections](size_t idx_a, size_t idx_b) {
                                               const auto &a = detections[idx_a];
                                               const auto &b = detections[idx_b];

                                               cv::Point2f center_a = (a.box.br() + a.box.tl()) * 0.5f;
                                               cv::Point2f center_b = (b.box.br() + b.box.tl()) * 0.5f;

                                               return cv::norm(center_a) < cv::norm(center_b);
                                           });

        const auto &first = detections[*min_idx_it];

        switch (first.class_id) {
            case detect_classes_t::TRAFFIC_LIGHT_GREEN:
            case detect_classes_t::COLOR_GREEN:
            case detect_classes_t::APPROVED_GREEN:
                situation.color = DetectedSituationColor::GREEN;
                break;
            case detect_classes_t::TRAFFIC_LIGHT_RED:
            case detect_classes_t::COLOR_RED:
            case detect_classes_t::APPROVED_RED:
                situation.color = DetectedSituationColor::RED;
                break;
            case detect_classes_t::TRAFFIC_LIGHT_RED_YELLOW:
            case detect_classes_t::TRAFFIC_LIGHT_YELLOW:
            case detect_classes_t::APPROVED_YELLOW:
            case detect_classes_t::APPROVED_RED_YELLOW:
                situation.color = DetectedSituationColor::YELLOW;
                break;

            default: ;
        }

        return situation;
    }
} // namespace detector_post_processing