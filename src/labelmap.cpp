#include "labelmap.h"
#include "seam.h"

#include <cstdint>
#include <print>

namespace labelmap {

// cv::Rect::area() is int and overflows on gigapixel-scale boxes.
static int64_t interArea(const cv::Rect& a, const cv::Rect& b) {
    const cv::Rect r = a & b;
    return static_cast<int64_t>(r.width) * r.height;
}

static double dist2(const cv::Rect& r, double cx, double cy) {
    const double dx = r.x + r.width  / 2.0 - cx;
    const double dy = r.y + r.height / 2.0 - cy;
    return dx * dx + dy * dy;
}

std::vector<int> placementOrder(const std::vector<cv::Rect>& rects) {
    const int N = static_cast<int>(rects.size());
    std::vector<int> order;
    order.reserve(N);
    if (N == 0) return order;

    double cx = 0.0, cy = 0.0;
    for (const auto& r : rects) {
        cx += r.x + r.width / 2.0;
        cy += r.y + r.height / 2.0;
    }
    cx /= N;
    cy /= N;

    std::vector<bool>    placed(N, false);
    std::vector<int64_t> best(N, 0);  // max overlap area vs the placed set

    while (static_cast<int>(order.size()) < N) {
        int pick = -1;
        for (int j = 0; j < N; ++j)
            if (!placed[j] && best[j] > 0 && (pick < 0 || best[j] > best[pick]))
                pick = j;
        if (pick < 0) {
            // First image, or a disconnected component: restart from the
            // remaining image closest to the global centroid.
            double bd = 0.0;
            for (int j = 0; j < N; ++j) {
                if (placed[j]) continue;
                const double d = dist2(rects[j], cx, cy);
                if (pick < 0 || d < bd) { pick = j; bd = d; }
            }
        }
        placed[pick] = true;
        order.push_back(pick);
        for (int j = 0; j < N; ++j)
            if (!placed[j])
                best[j] = std::max(best[j], interArea(rects[pick], rects[j]));
    }
    return order;
}

cv::Mat accumulate(const std::vector<cv::Mat>& images,
                   const std::vector<cv::Rect>& rects,
                   const std::vector<int>& order,
                   bool grayscale,
                   const StepCallback& on_step) {
    const size_t N = images.size();
    CV_Assert(N > 0 && rects.size() == N && order.size() == N);
    const cv::Size canvas = images[0].size();
    const cv::Rect canvas_rect(0, 0, canvas.width, canvas.height);

    cv::Mat label(canvas, CV_16UC1, cv::Scalar(0));
    // Hard-cut composite of original pixels: each pixel holds its current
    // owner's value, so a newcomer is always cut against exactly the content
    // it will neighbour. One canvas, updated in place — never blended.
    cv::Mat mosaic(canvas, CV_32FC4, cv::Scalar(0, 0, 0, 0));

    // Give idx's opaque pixels to idx where the mask says newcomer (or
    // unconditionally when mask is null, for the first image).
    auto claim = [&](int idx, const cv::Mat* mask) {
        const cv::Rect roi = rects[idx] & canvas_rect;
        const cv::Mat& img = images[idx];
        for (int y = roi.y; y < roi.y + roi.height; ++y) {
            const cv::Vec4f* src = img.ptr<cv::Vec4f>(y);
            const uint8_t*   m   = mask ? mask->ptr<uint8_t>(y) : nullptr;
            cv::Vec4f*       dst = mosaic.ptr<cv::Vec4f>(y);
            uint16_t*        lbl = label.ptr<uint16_t>(y);
            for (int x = roi.x; x < roi.x + roi.width; ++x) {
                if (src[x][3] <= 0.5f) continue;
                if (m && m[x] != 255) continue;
                dst[x] = src[x];
                lbl[x] = static_cast<uint16_t>(idx + 1);
            }
        }
    };

    claim(order[0], nullptr);

    for (size_t k = 1; k < N; ++k) {
        const int idx = order[k];
        if ((rects[idx] & canvas_rect).empty()) continue;  // entirely off-canvas

        // Pixel overlap with the mosaic can only occur where the newcomer's
        // box intersects an already-placed box — union those intersections
        // and compute the error there only.
        cv::Rect roi;
        for (size_t p = 0; p < k; ++p)
            roi |= (rects[order[p]] & rects[idx]);
        roi &= canvas_rect;
        if (roi.empty()) {
            // Disconnected from everything placed so far: no cut to make,
            // the newcomer just claims its own coverage.
            claim(idx, nullptr);
            continue;
        }
        std::println("  Seam {}/{}: image {} vs mosaic", k, N - 1, idx);

        cv::Mat err(canvas, CV_32FC1, cv::Scalar(seam::kNoOverlap));
        cv::Mat err_roi = err(roi);
        seam::computeError(mosaic(roi), images[idx](roi), err_roi, grayscale);

        const cv::Mat mask = seam::findSeam(mosaic, images[idx], err);
        if (on_step) on_step(static_cast<int>(k), idx, err, mask, mosaic);
        claim(idx, &mask);
    }
    return label;
}

} // namespace labelmap
