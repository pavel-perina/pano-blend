#include "seam.h"
#include "colors.h"

#include <opencv2/imgproc/detail/gcgraph.hpp>
#include <cmath>

namespace seam {

// Sentinel for pixels outside the overlap — finite, larger than any real
// OKLab distance, safe for downstream squaring in n-weight computation.
static constexpr float kNoOverlap = 1e6f;

cv::Mat computeError(const cv::Mat& f1, const cv::Mat& f2) {
    const int h = f1.rows;
    const int w = f1.cols;
    cv::Mat err(h, w, CV_32FC1);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto p1 = f1.at<cv::Vec4f>(y, x);
            const auto p2 = f2.at<cv::Vec4f>(y, x);

            // OpenCV BGRA order: [0]=B [1]=G [2]=R [3]=A
            if (p1[3] < 0.5f || p2[3] < 0.5f) {
                err.at<float>(y, x) = kNoOverlap;
                continue;
            }

            const auto lab1 = color::okLabFromRgb({p1[2], p1[1], p1[0]});
            const auto lab2 = color::okLabFromRgb({p2[2], p2[1], p2[0]});

            const float dL = lab1.L - lab2.L;
            const float da = lab1.a - lab2.a;
            const float db = lab1.b - lab2.b;
            err.at<float>(y, x) = std::sqrt(dL*dL + da*da + db*db);
        }
    }
    return err;
}

cv::Mat findSeam(const cv::Mat& f1, const cv::Mat& f2, const cv::Mat& err) {
    const int h = f1.rows;
    const int w = f1.cols;

    // Pre-allocate: w*h nodes, up to 2 undirected edges per pixel (right+down)
    // → each addEdges call adds 2 directed edges, so reserve 4*w*h slots.
    cv::detail::GCGraph<float> graph;
    graph.create(h * w, 4 * h * w);

    for (int i = 0; i < h * w; ++i)
        graph.addVtx();

    constexpr float kHard = 1e6f;  // terminal capacity for single-image pixels

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int idx = y * w + x;
            const bool in1 = f1.at<cv::Vec4f>(y, x)[3] > 0.5f;
            const bool in2 = f2.at<cv::Vec4f>(y, x)[3] > 0.5f;

            // T-weights — hard-pin single-image pixels to their terminal
            if (in1 && !in2)
                graph.addTermWeights(idx, kHard, 0.0f);
            else if (!in1 && in2)
                graph.addTermWeights(idx, 0.0f, kHard);

            // N-weights: add right and down edges whenever at least one endpoint
            // is in the overlap — this connects the overlap to both terminal sides.
            //   both overlap      → OKLab cost (seam prefers where images agree)
            //   overlap + single  → kHard     (keeps seam inside the overlap)
            //   neither in overlap → no edge
            const bool overlap = in1 && in2;
            const float eu = overlap ? err.at<float>(y, x) : 0.0f;

            auto tryEdge = [&](int ny, int nx, int nidx) {
                const bool n1 = f1.at<cv::Vec4f>(ny, nx)[3] > 0.5f;
                const bool n2 = f2.at<cv::Vec4f>(ny, nx)[3] > 0.5f;
                const bool n_overlap = n1 && n2;
                if (!overlap && !n_overlap)     return;  // neither in overlap
                if (!(in1 || in2) || !(n1 || n2)) return;  // endpoint outside all images
                const float ev = n_overlap ? err.at<float>(ny, nx) : 0.0f;
                const float cap = (overlap && n_overlap) ? eu*eu + ev*ev : kHard;
                graph.addEdges(idx, nidx, cap, cap);
            };

            if (x + 1 < w) tryEdge(y,     x + 1, idx + 1);
            if (y + 1 < h) tryEdge(y + 1, x,     idx + w);
        }
    }

    graph.maxFlow();

    cv::Mat mask(h, w, CV_8UC1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            mask.at<uint8_t>(y, x) =
                graph.inSourceSegment(y * w + x) ? 0 : 255;
        }
    }
    return mask;
}

cv::Mat visualizeSeam(const cv::Mat& f1, const cv::Mat& f2,
                      const cv::Mat& err, const cv::Mat& mask) {
    const int h = f1.rows;
    const int w = f1.cols;
    cv::Mat out(h, w, CV_8UC4);

    constexpr float kC   = 0.08f;
    constexpr float kH1  = 80.0f;   // orange — image1 side
    constexpr float kH2  = 260.0f;  // blue   — image2 side
    constexpr float kLSingle = 0.40f; // lightness for single-image regions

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool in1 = f1.at<cv::Vec4f>(y, x)[3] > 0.5f;
            const bool in2 = f2.at<cv::Vec4f>(y, x)[3] > 0.5f;

            if (!in1 && !in2) {
                out.at<cv::Vec4b>(y, x) = {0, 0, 0, 0};
                continue;
            }

            float L, H;
            if (in1 && in2) {
                const float e = err.at<float>(y, x);
                L = 0.2f + 0.6f * std::min(e * 3.0f, 1.0f);
                H = (mask.at<uint8_t>(y, x) == 0) ? kH1 : kH2;
            } else {
                L = kLSingle;
                H = in1 ? kH1 : kH2;
            }

            const auto lab = color::okLchToLab({L, kC, H});
            const auto rgb = color::okLabToRgb(lab);
            const uint8_t R = static_cast<uint8_t>(std::clamp(rgb.r, 0.0f, 1.0f) * 255.0f);
            const uint8_t G = static_cast<uint8_t>(std::clamp(rgb.g, 0.0f, 1.0f) * 255.0f);
            const uint8_t B = static_cast<uint8_t>(std::clamp(rgb.b, 0.0f, 1.0f) * 255.0f);
            out.at<cv::Vec4b>(y, x) = {B, G, R, 255};
        }
    }
    return out;
}

} // namespace seam
