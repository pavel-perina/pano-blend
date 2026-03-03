#pragma once

#include <opencv2/core.hpp>

namespace blend {

// Multi-band blend of two float BGRA images using their seam mask.
// seam_mask: CV_8UC1 from seam::findSeam (0=image1, 255=image2)
// Returns CV_8UC4 BGRA.
cv::Mat multiBandBlend(const cv::Mat& f1, const cv::Mat& f2,
                       const cv::Mat& seam_mask, int num_bands = 5);

} // namespace blend
