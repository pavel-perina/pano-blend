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

cv::Mat accumulate(const std::vector<cv::Mat>& crops,
                   const std::vector<cv::Rect>& rects,
                   cv::Size canvas,
                   const std::vector<int>& order,
                   bool grayscale,
                   const StepCallback& on_step) {
    const size_t N = crops.size();
    CV_Assert(N > 0 && rects.size() == N && order.size() == N);
    for (size_t i = 0; i < N; ++i)
        CV_Assert(crops[i].size() == rects[i].size() && crops[i].type() == CV_32FC4);
    const cv::Rect canvas_rect(0, 0, canvas.width, canvas.height);

    cv::Mat label(canvas, CV_16UC1, cv::Scalar(0));
    // Hard-cut composite of original pixels: each pixel holds its current
    // owner's value, so a newcomer is always cut against exactly the content
    // it will neighbour. The only full-canvas float buffer — never blended.
    cv::Mat mosaic(canvas, CV_32FC4, cv::Scalar(0, 0, 0, 0));

    // Give idx's opaque pixels to idx where the view-local mask says
    // newcomer (or unconditionally when mask is null, for the first image).
    auto claim = [&](int idx, const cv::Mat* mask, cv::Rect view) {
        const cv::Rect roi = rects[idx] & canvas_rect;
        for (int y = roi.y; y < roi.y + roi.height; ++y) {
            const cv::Vec4f* src = crops[idx].ptr<cv::Vec4f>(y - rects[idx].y);
            const uint8_t*   m   = mask ? mask->ptr<uint8_t>(y - view.y) : nullptr;
            cv::Vec4f*       dst = mosaic.ptr<cv::Vec4f>(y);
            uint16_t*        lbl = label.ptr<uint16_t>(y);
            for (int x = roi.x; x < roi.x + roi.width; ++x) {
                if (src[x - rects[idx].x][3] <= 0.5f) continue;
                if (m && m[x - view.x] != 255) continue;
                dst[x] = src[x - rects[idx].x];
                lbl[x] = static_cast<uint16_t>(idx + 1);
            }
        }
    };

    claim(order[0], nullptr, {});

    for (size_t k = 1; k < N; ++k) {
        const int idx = order[k];
        if ((rects[idx] & canvas_rect).empty()) continue;  // entirely off-canvas

        // Pixel overlap with the mosaic can only occur where the newcomer's
        // box intersects an already-placed box — union those intersections
        // and compute the error there only.
        cv::Rect overlap;
        for (size_t p = 0; p < k; ++p)
            overlap |= (rects[order[p]] & rects[idx]);
        overlap &= canvas_rect;
        if (overlap.empty()) {
            // Disconnected from everything placed so far: no cut to make,
            // the newcomer just claims its own coverage.
            claim(idx, nullptr, {});
            continue;
        }
        std::println("  Seam {}/{}: image {} vs mosaic", k, N - 1, idx);

        // Seam work happens on a view of the canvas: the newcomer's box plus
        // a kCoarseScale ring so single-image territory survives the coarse
        // downsample and anchors the cut's hard T-weights.
        constexpr int m = seam::kCoarseScale;
        const cv::Rect view = cv::Rect(rects[idx].x - m, rects[idx].y - m,
                                       rects[idx].width + 2 * m,
                                       rects[idx].height + 2 * m) & canvas_rect;

        // Place the newcomer crop into a view-sized image.
        cv::Mat newcomer(view.size(), CV_32FC4, cv::Scalar(0, 0, 0, 0));
        const cv::Rect on_canvas = rects[idx] & canvas_rect;
        crops[idx](cv::Rect(on_canvas.tl() - rects[idx].tl(), on_canvas.size()))
            .copyTo(newcomer(cv::Rect(on_canvas.tl() - view.tl(), on_canvas.size())));

        cv::Mat err(view.size(), CV_32FC1, cv::Scalar(seam::kNoOverlap));
        cv::Mat err_roi = err(cv::Rect(overlap.tl() - view.tl(), overlap.size()));
        seam::computeError(mosaic(overlap),
                           newcomer(cv::Rect(overlap.tl() - view.tl(), overlap.size())),
                           err_roi, grayscale);

        const cv::Mat mosaic_view = mosaic(view);
        const cv::Mat mask = seam::findSeam(mosaic_view, newcomer, err);
        if (on_step)
            on_step({static_cast<int>(k), idx, view, err, mask, mosaic_view, newcomer});
        claim(idx, &mask, view);
    }
    return label;
}

} // namespace labelmap
