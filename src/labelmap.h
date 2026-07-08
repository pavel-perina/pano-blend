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

// Called once per cut, before it is applied. step is 1..N-1, image the index
// being placed, mask the seam (0 = accumulated mosaic side, 255 = newcomer),
// mosaic the hard-cut composite the newcomer was cut against.
using StepCallback = std::function<void(int step, int image, const cv::Mat& err,
                                        const cv::Mat& mask, const cv::Mat& mosaic)>;

// Sequential-accumulate label map: place images[order[0]], then graph-cut each
// images[order[k]] against the accumulated coverage of the previously placed
// images — N-1 cuts. The returned label map (CV_16UC1; 0 = uncovered,
// 1..N = image index) is the memo of that sequence: cut winners take the
// newcomer's label, losers keep theirs.
// images: canvas-sized CV_32FC4; rects: each image's bounding box in canvas
// coordinates (may extend past the canvas; used to restrict per-step work).
// order: a permutation of 0..N-1, e.g. from placementOrder().
cv::Mat accumulate(const std::vector<cv::Mat>& images,
                   const std::vector<cv::Rect>& rects,
                   const std::vector<int>& order,
                   bool grayscale = false,
                   const StepCallback& on_step = nullptr);

} // namespace labelmap
