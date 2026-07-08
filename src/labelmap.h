#pragma once

#include <opencv2/core.hpp>
#include <functional>
#include <vector>

namespace labelmap {

// Deterministic placement order over the images' canvas bounding boxes:
// Prim's maximum-overlap spanning tree rooted at the image closest to the
// centroid of all bounding-box centers. At each step the unplaced image with
// the largest bbox-intersection area against the placed set is added (ties
// broken by lowest index). If no unplaced image overlaps the placed set
// (disconnected panorama), growth restarts from the most central remaining
// image. bbox area is only the ordering weight — never a seam decision.
std::vector<int> placementOrder(const std::vector<cv::Rect>& rects);

// One accumulate step, handed to the callback before the cut is applied.
// All mats are view-local; |view| places them on the canvas.
struct Step {
    int step;               // 1..N-1
    int image;              // index of the image being placed
    cv::Rect view;          // canvas rect the mats below correspond to
    const cv::Mat& err;     // CV_32FC1 error (kNoOverlap outside the overlap)
    const cv::Mat& mask;    // CV_8UC1 cut: 0 = mosaic side, 255 = newcomer
    const cv::Mat& mosaic;  // CV_32FC4 composite the newcomer was cut against
    const cv::Mat& newcomer;// CV_32FC4 the placed newcomer
};
using StepCallback = std::function<void(const Step&)>;

// Sequential-accumulate label map: place crops[order[0]], then graph-cut each
// crops[order[k]] against the accumulated coverage of the previously placed
// images — N-1 cuts. The returned label map (CV_16UC1; 0 = uncovered,
// 1..N = image index) is the memo of that sequence: cut winners take the
// newcomer's label, losers keep theirs.
// crops: CV_32FC4 image crops; rects: each crop's placement in canvas
// coordinates (rects[i].size() == crops[i].size(); may extend past the
// canvas). order: a permutation of 0..N-1, e.g. from placementOrder().
// Working memory is one full-canvas CV_32FC4 mosaic plus the label map;
// per-step buffers are view-sized (newcomer rect + margin).
cv::Mat accumulate(const std::vector<cv::Mat>& crops,
                   const std::vector<cv::Rect>& rects,
                   cv::Size canvas,
                   const std::vector<int>& order,
                   bool grayscale = false,
                   const StepCallback& on_step = nullptr);

} // namespace labelmap
