#pragma once

#include <opencv2/core.hpp>
#include <vector>

namespace blend {

// Multi-band blend of N float BGRA image crops using a label map.
// crops: CV_32FC4 image crops; rects: each crop's placement in canvas
// coordinates (rects[i].size() == crops[i].size()).
// label_map: canvas-sized CV_16UC1, 0=no image, 1..N=winning image (1-based).
// Returns canvas-sized CV_8UC4 BGRA.
cv::Mat multiBandBlend(const std::vector<cv::Mat>& crops,
                       const std::vector<cv::Rect>& rects,
                       const cv::Mat& label_map, int num_bands = 5);

} // namespace blend
