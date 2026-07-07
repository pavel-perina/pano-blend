#pragma once

#include <opencv2/core.hpp>
#include <vector>

namespace blend {

// Multi-band blend of N float BGRA images using a label map.
// label_map: CV_16UC1, 0=no image, 1..N=winning image index (1-based).
// images: vector of N canvas-sized CV_32FC4 float BGRA images.
// Returns CV_8UC4 BGRA.
cv::Mat multiBandBlend(const std::vector<cv::Mat>& images,
                       const cv::Mat& label_map, int num_bands = 5);

} // namespace blend
