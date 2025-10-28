#include "sahi.h"
#include <map>
#include <algorithm>
#include <numeric>

// Helper function to calculate IoU between two rectangles
float calculate_iou(const cv::Rect& box1, const cv::Rect& box2) {
    int x1 = std::max(box1.x, box2.x);
    int y1 = std::max(box1.y, box2.y);
    int x2 = std::min(box1.x + box1.width, box2.x + box2.width);
    int y2 = std::min(box1.y + box1.height, box2.y + box2.height);
    
    if (x2 <= x1 || y2 <= y1) {
        return 0.0f;
    }
    
    int intersection_area = (x2 - x1) * (y2 - y1);
    int box1_area = box1.width * box1.height;
    int box2_area = box2.width * box2.height;
    int union_area = box1_area + box2_area - intersection_area;
    
    return static_cast<float>(intersection_area) / static_cast<float>(union_area);
}


SAHI::SAHI(
    OBDet* detector,
    cv::Size model_input_size,
    float overlap_ratio,
    float nms_threshold
    )
    : detector_(detector),
      model_input_size_(model_input_size),
      overlap_ratio_(overlap_ratio),
      nms_threshold_(nms_threshold)
{
    if (!detector_) {
        throw std::invalid_argument("Detector cannot be null");
    }
    set_overlap_ratio(overlap_ratio); // Apply clamping
}

std::vector<Detection> SAHI::detect(const cv::Mat& image) {
    // Get slice size from stored model input size
    cv::Size slice_size = model_input_size_;
    
    // For small images, use direct detection
    if (image.cols <= slice_size.width && image.rows <= slice_size.height) {
        // Use OBDet's detection workflow
        detector_->pre_process(image);
        detector_->inference();
        
        std::vector<Detection> results;
        detector_->post_process({static_cast<size_t>(image.cols), static_cast<size_t>(image.rows)}, results);
        return results;
    }
    
    // Use streaming approach - process slices immediately without storing them
    std::vector<Detection> all_detections;
    
    int overlap_width = static_cast<int>(slice_size.width * overlap_ratio_);
    int overlap_height = static_cast<int>(slice_size.height * overlap_ratio_);
    int stride_x = slice_size.width - overlap_width;
    int stride_y = slice_size.height - overlap_height;

    auto duration_sum = std::chrono::steady_clock::duration::zero();
    
    // Process slices immediately without storing them
    int slice_counter = 1;
    for (int y = 0; y < image.rows; y += stride_y) {
        for (int x = 0; x < image.cols; x += stride_x) {
            // Calculate slice boundaries and scale factors upfront
            int x_end = std::min(x + slice_size.width, image.cols);
            int y_end = std::min(y + slice_size.height, image.rows);
            
            // Calculate original region size
            int original_width = x_end - x;
            int original_height = y_end - y;
            
            // Calculate scale factors
            float scale_x = static_cast<float>(original_width) / slice_size.width;
            float scale_y = static_cast<float>(original_height) / slice_size.height;
            
            // Create region that will be extracted at the exact slice_size
            cv::Rect region(x, y, original_width, original_height);
            cv::Mat slice = image(region);
            
            // Only resize if the slice is not already the correct size
            if (slice.cols != slice_size.width || slice.rows != slice_size.height) {
                cv::resize(slice, slice, slice_size);
            }
            
            // Save slice before detection (disabled - uncomment to enable)
             /*std::string slice_filename = "slides/" + std::to_string(slice_counter) + ".jpg";
             cv::imwrite(slice_filename, slice);
             slice_counter++;*/

            auto m_start = std::chrono::steady_clock::now();
            // DETECT IMMEDIATELY using OBDet workflow - no storage needed
            detector_->pre_process(slice);
            detector_->inference();
            
            std::vector<Detection> slice_results;
            detector_->post_process({static_cast<size_t>(slice_size.width), static_cast<size_t>(slice_size.height)}, slice_results);
            auto duration = std::chrono::steady_clock::now() - m_start;
            duration_sum += duration;
            
            // Transform coordinates to original image space
            for (auto& det : slice_results) {
                // Update the bounding box coordinates
                det.box.x = region.x + static_cast<int>(det.box.x * scale_x);
                det.box.y = region.y + static_cast<int>(det.box.y * scale_y);
                det.box.width = static_cast<int>(det.box.width * scale_x);
                det.box.height = static_cast<int>(det.box.height * scale_y);
                
                all_detections.push_back(det);
            }
            
            // slice goes out of scope here - memory automatically freed
        }
    }
    
    // Print total detection timing
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration_sum).count();
    std::cout << "Total SAHI detection time: " << total_ms << " ms" << std::endl;
    
    // Merge overlapping detections
    auto merged_results = merge_detections(all_detections);
    
    return merged_results;
}


std::vector<Detection> SAHI::merge_detections(
    const std::vector<Detection>& all_detections) const {
    
    if (all_detections.empty()) {
        return {};
    }
    
    // Group detections by class for class-wise NMS
    std::map<int, std::vector<size_t>> class_groups;
    for (size_t i = 0; i < all_detections.size(); ++i) {
        class_groups[all_detections[i].class_id].push_back(i);
    }
    
    std::vector<Detection> merged_results;
    
    // Apply NMS for each class separately
    for (const auto& [class_name, indices] : class_groups) {
        std::vector<cv::Rect> boxes;
        std::vector<float> confidences;
        
        for (size_t idx : indices) {
            const auto& det = all_detections[idx];
            boxes.push_back(det.box);
            confidences.push_back(det.confidence);
        }
        
        // Apply custom NMS implementation
        std::vector<bool> suppressed(boxes.size(), false);
        
        // Sort by confidence in descending order
        std::vector<size_t> sorted_indices(boxes.size());
        std::iota(sorted_indices.begin(), sorted_indices.end(), 0);
        std::sort(sorted_indices.begin(), sorted_indices.end(), 
                  [&confidences](size_t a, size_t b) {
                      return confidences[a] > confidences[b];
                  });
        
        // Apply NMS
        for (size_t i = 0; i < sorted_indices.size(); ++i) {
            size_t idx_i = sorted_indices[i];
            if (suppressed[idx_i]) continue;
            
            for (size_t j = i + 1; j < sorted_indices.size(); ++j) {
                size_t idx_j = sorted_indices[j];
                if (suppressed[idx_j]) continue;
                
                // Calculate IoU
                float iou = calculate_iou(boxes[idx_i], boxes[idx_j]);
                if (iou > nms_threshold_) {
                    suppressed[idx_j] = true;
                }
            }
        }
        
        // Add non-suppressed detections to final results
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (!suppressed[i]) {
                merged_results.push_back(all_detections[indices[i]]);
            }
        }
    }
    
    return merged_results;
}

void SAHI::set_overlap_ratio(float ratio) {
    overlap_ratio_ = std::clamp(ratio, 0.0f, 0.5f);
}
