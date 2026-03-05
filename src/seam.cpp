#include "seam.h"
#include "colors.h"

#include <opencv2/imgproc/detail/gcgraph.hpp>
#include <cmath>
#include <print>

namespace seam {

// Sentinel for pixels outside the overlap — finite, larger than any real
// OKLab distance, safe for downstream squaring in n-weight computation.
static constexpr float kNoOverlap = 1e6f;

cv::Mat computeError(const cv::Mat& f1, const cv::Mat& f2) {
    const int h = f1.rows;
    const int w = f1.cols;
    cv::Mat err(h, w, CV_32FC1);

    for (int y = 0; y < h; ++y) {
        const cv::Vec4f* r1 = f1.ptr<cv::Vec4f>(y);
        const cv::Vec4f* r2 = f2.ptr<cv::Vec4f>(y);
        float*           re = err.ptr<float>(y);
        for (int x = 0; x < w; ++x) {
            if (r1[x][3] < 0.5f || r2[x][3] < 0.5f) {
                re[x] = kNoOverlap;
                continue;
            }
            // OpenCV BGRA: [0]=B [1]=G [2]=R [3]=A
            const auto lab1 = color::okLabFromRgb({r1[x][2], r1[x][1], r1[x][0]});
            const auto lab2 = color::okLabFromRgb({r2[x][2], r2[x][1], r2[x][0]});
            const float dL = lab1.L - lab2.L;
            const float da = lab1.a - lab2.a;
            const float db = lab1.b - lab2.b;
            re[x] = std::sqrt(dL*dL + da*da + db*db);
        }
    }
    return err;
}

cv::Mat findSeam(const cv::Mat& f1, const cv::Mat& f2, const cv::Mat& err) {
    const int H = f1.rows;
    const int W = f1.cols;

    // --- Find bounding box of the overlap region ---
    int ox0 = W, oy0 = H, ox1 = 0, oy1 = 0;
    for (int y = 0; y < H; ++y) {
        const cv::Vec4f* r1 = f1.ptr<cv::Vec4f>(y);
        const cv::Vec4f* r2 = f2.ptr<cv::Vec4f>(y);
        for (int x = 0; x < W; ++x) {
            if (r1[x][3] > 0.5f && r2[x][3] > 0.5f) {
                if (x   < ox0) ox0 = x;
                if (x+1 > ox1) ox1 = x + 1;
                if (y   < oy0) oy0 = y;
                if (y+1 > oy1) oy1 = y + 1;
            }
        }
    }

    if (ox0 >= ox1 || oy0 >= oy1) {
        // No overlap — seam at left edge, full image1
        return cv::Mat::zeros(H, W, CV_8UC1);
    }

    // Expand by 1 pixel to include the single-image border pixels
    ox0 = std::max(0, ox0 - 1);
    oy0 = std::max(0, oy0 - 1);
    ox1 = std::min(W, ox1 + 1);
    oy1 = std::min(H, oy1 + 1);

    const int cw = ox1 - ox0;
    const int ch = oy1 - oy0;
    std::println("  Overlap crop: {}x{} at ({},{}) [canvas {}x{}]", cw, ch, ox0, oy0, W, H);

    // --- Build BK graph on the crop ---
    cv::detail::GCGraph<float> graph;
    graph.create(ch * cw, 4 * ch * cw);
    for (int i = 0; i < ch * cw; ++i) graph.addVtx();

    constexpr float kHard = 1e6f;

    for (int cy = 0; cy < ch; ++cy) {
        const int y = cy + oy0;
        const cv::Vec4f* r1 = f1.ptr<cv::Vec4f>(y);
        const cv::Vec4f* r2 = f2.ptr<cv::Vec4f>(y);
        const float*     re = err.ptr<float>(y);

        for (int cx = 0; cx < cw; ++cx) {
            const int x       = cx + ox0;
            const int idx     = cy * cw + cx;
            const bool in1    = r1[x][3] > 0.5f;
            const bool in2    = r2[x][3] > 0.5f;
            const bool overlap = in1 && in2;

            if (in1 && !in2)
                graph.addTermWeights(idx, kHard, 0.0f);
            else if (!in1 && in2)
                graph.addTermWeights(idx, 0.0f, kHard);

            const float eu = overlap ? re[x] : 0.0f;

            // Right neighbour
            if (cx + 1 < cw) {
                const int nx  = x + 1;
                const int nidx = idx + 1;
                const bool n1 = r1[nx][3] > 0.5f;
                const bool n2 = r2[nx][3] > 0.5f;
                const bool n_overlap = n1 && n2;
                if (overlap || n_overlap) {
                    if ((in1 || in2) && (n1 || n2)) {
                        const float ev  = n_overlap ? re[nx] : 0.0f;
                        const float cap = (overlap && n_overlap) ? eu*eu + ev*ev : kHard;
                        graph.addEdges(idx, nidx, cap, cap);
                    }
                }
            }

            // Down neighbour
            if (cy + 1 < ch) {
                const int ny   = y + 1;
                const int nidx = idx + cw;
                const cv::Vec4f* nr1 = f1.ptr<cv::Vec4f>(ny);
                const cv::Vec4f* nr2 = f2.ptr<cv::Vec4f>(ny);
                const float*     nre = err.ptr<float>(ny);
                const bool n1 = nr1[x][3] > 0.5f;
                const bool n2 = nr2[x][3] > 0.5f;
                const bool n_overlap = n1 && n2;
                if (overlap || n_overlap) {
                    if ((in1 || in2) && (n1 || n2)) {
                        const float ev  = n_overlap ? nre[x] : 0.0f;
                        const float cap = (overlap && n_overlap) ? eu*eu + ev*ev : kHard;
                        graph.addEdges(idx, nidx, cap, cap);
                    }
                }
            }
        }
    }

    graph.maxFlow();

    // --- Build full-size mask ---
    cv::Mat mask(H, W, CV_8UC1);
    for (int y = 0; y < H; ++y) {
        const cv::Vec4f* r1 = f1.ptr<cv::Vec4f>(y);
        const cv::Vec4f* r2 = f2.ptr<cv::Vec4f>(y);
        uint8_t*         rm = mask.ptr<uint8_t>(y);
        const bool in_crop_y = (y >= oy0 && y < oy1);

        for (int x = 0; x < W; ++x) {
            if (in_crop_y && x >= ox0 && x < ox1) {
                rm[x] = graph.inSourceSegment((y - oy0) * cw + (x - ox0)) ? 0 : 255;
            } else {
                // Outside crop: all single-image pixels
                rm[x] = (r2[x][3] > 0.5f && r1[x][3] <= 0.5f) ? 255 : 0;
            }
        }
    }
    return mask;
}

cv::Mat visualizeSeam(const cv::Mat& f1, const cv::Mat& f2,
                      const cv::Mat& err, const cv::Mat& mask) {
    const int h = f1.rows;
    const int w = f1.cols;
    cv::Mat out(h, w, CV_8UC4);

    constexpr float kC       = 0.08f;
    constexpr float kH1      = 80.0f;   // orange — image1 side
    constexpr float kH2      = 260.0f;  // blue   — image2 side
    constexpr float kLSingle = 0.40f;

    for (int y = 0; y < h; ++y) {
        const cv::Vec4f* r1 = f1.ptr<cv::Vec4f>(y);
        const cv::Vec4f* r2 = f2.ptr<cv::Vec4f>(y);
        const float*     re = err.ptr<float>(y);
        const uint8_t*   rm = mask.ptr<uint8_t>(y);
        cv::Vec4b*       ro = out.ptr<cv::Vec4b>(y);

        for (int x = 0; x < w; ++x) {
            const bool in1 = r1[x][3] > 0.5f;
            const bool in2 = r2[x][3] > 0.5f;

            if (!in1 && !in2) { ro[x] = {0, 0, 0, 0}; continue; }

            float L, H;
            if (in1 && in2) {
                L = 0.2f + 0.6f * std::min(re[x] * 3.0f, 1.0f);
                H = (rm[x] == 0) ? kH1 : kH2;
            } else {
                L = kLSingle;
                H = in1 ? kH1 : kH2;
            }

            const auto lab = color::okLchToLab({L, kC, H});
            const auto rgb = color::okLabToRgb(lab);
            const uint8_t R = static_cast<uint8_t>(std::clamp(rgb.r, 0.0f, 1.0f) * 255.0f);
            const uint8_t G = static_cast<uint8_t>(std::clamp(rgb.g, 0.0f, 1.0f) * 255.0f);
            const uint8_t B = static_cast<uint8_t>(std::clamp(rgb.b, 0.0f, 1.0f) * 255.0f);
            ro[x] = {B, G, R, 255};
        }
    }
    return out;
}

} // namespace seam
