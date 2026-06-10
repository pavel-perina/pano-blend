#pragma once

#include <opencv2/core.hpp>

namespace seam {

// Sentinel for pixels outside the overlap — finite, larger than any real
// OKLab distance, safe for downstream squaring in n-weight computation.
inline constexpr float kNoOverlap = 1e6f;

// Per-pixel distance between two float BGRA images (CV_32FC4).
// Color images: OKLab perceptual distance. Grayscale: absolute intensity diff.
// Pixels not covered by both images are filled with kNoOverlap.
// err must be preallocated CV_32FC1 of the same size (ROI views are fine).
void computeError(const cv::Mat& f1, const cv::Mat& f2, cv::Mat& err,
                  bool grayscale = false);

// Boykov-Kolmogorov graph-cut seam.
// Returns CV_8UC1 mask: 0 = image1 (source side), 255 = image2 (sink side).
// Outside the overlap, single-image pixels keep their own side; if the two
// images share no pixels at all, the mask is just the coverage split.
cv::Mat findSeam(const cv::Mat& f1, const cv::Mat& f2, const cv::Mat& err);

// False-colour diagnostic: OKLCh with error as lightness.
//   image1-only → dim orange (H=80), image2-only → dim blue (H=260)
//   overlap     → bright = high error; hue = which side of the seam won
// Returns CV_8UC4 BGRA.
cv::Mat visualizeSeam(const cv::Mat& f1, const cv::Mat& f2,
                      const cv::Mat& err, const cv::Mat& mask);

} // namespace seam
