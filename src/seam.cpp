#include "seam.h"
#include "colors.h"

#include <opencv2/imgproc.hpp>
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

    // --- Distance-based T-weights for overlap pixels (analogous to SmartBlend's DER/DEC) ---
    //
    // SmartBlend assigns small terminal weights to overlap pixels based on their
    // distance from the img1/img2 coverage boundaries.  Pixels close to img1
    // territory get a small positive T-weight (source bias) and vice versa.
    // This seeds overlap pixels directly into BK trees, so no kHard N-weight
    // edges at the overlap boundary are needed — the total flow is then bounded
    // by the N-weight capacities, making BK very fast.
    //
    // kBias: T-weight per pixel of distance difference (img1 vs img2 boundary).
    // Must be much smaller than typical N-weight so N-weights dominate seam choice.
    constexpr float kBias = 1e-4f;

    // Build single-image masks over the crop
    cv::Mat mask_in1(ch, cw, CV_8U, cv::Scalar(0));
    cv::Mat mask_in2(ch, cw, CV_8U, cv::Scalar(0));
    for (int cy = 0; cy < ch; ++cy) {
        const cv::Vec4f* r1 = f1.ptr<cv::Vec4f>(cy + oy0);
        const cv::Vec4f* r2 = f2.ptr<cv::Vec4f>(cy + oy0);
        uint8_t* m1 = mask_in1.ptr<uint8_t>(cy);
        uint8_t* m2 = mask_in2.ptr<uint8_t>(cy);
        for (int cx = 0; cx < cw; ++cx) {
            const int x = cx + ox0;
            const bool in1 = r1[x][3] > 0.5f;
            const bool in2 = r2[x][3] > 0.5f;
            m1[cx] = (in1 && !in2) ? 255 : 0;
            m2[cx] = (!in1 && in2) ? 255 : 0;
        }
    }

    // dist1f[cy][cx] = distance (pixels) from (cx,cy) to nearest img1-only pixel
    // dist2f[cy][cx] = distance (pixels) from (cx,cy) to nearest img2-only pixel
    // distanceTransform computes: for each non-zero pixel, distance to nearest zero.
    // Input  = 255 - mask  → zeros mark the region we want distance FROM.
    cv::Mat dist1f, dist2f;
    cv::distanceTransform(255 - mask_in1, dist1f, cv::DIST_L2, cv::DIST_MASK_PRECISE);
    cv::distanceTransform(255 - mask_in2, dist2f, cv::DIST_L2, cv::DIST_MASK_PRECISE);

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
        const float*     d1 = dist1f.ptr<float>(cy);
        const float*     d2 = dist2f.ptr<float>(cy);

        for (int cx = 0; cx < cw; ++cx) {
            const int x       = cx + ox0;
            const int idx     = cy * cw + cx;
            const bool in1    = r1[x][3] > 0.5f;
            const bool in2    = r2[x][3] > 0.5f;
            const bool overlap = in1 && in2;

            // T-weight: positive = source (img1 side), negative = sink (img2 side)
            if (in1 && !in2) {
                graph.addTermWeights(idx, kHard, 0.0f);
            } else if (!in1 && in2) {
                graph.addTermWeights(idx, 0.0f, kHard);
            } else if (overlap) {
                // Distance-based bias: positive near img1 boundary, negative near img2
                const float tw = kBias * (d2[cx] - d1[cx]);
                if      (tw > 0.0f) graph.addTermWeights(idx, tw, 0.0f);
                else if (tw < 0.0f) graph.addTermWeights(idx, 0.0f, -tw);
                // tw == 0 (equidistant): free node, assigned by cut structure
            }
            // else: outside both images → free node, irrelevant to seam

            // N-weights: only between doubly-covered (overlap) pixels.
            // No kHard edges at the overlap boundary — single-image pixels are
            // hard-pinned by their T-weights, so the cut stays inside the overlap.
            if (!overlap) continue;

            const float eu = re[x];

            // Right neighbour
            if (cx + 1 < cw) {
                const bool n_in1 = r1[x+1][3] > 0.5f;
                const bool n_in2 = r2[x+1][3] > 0.5f;
                if (n_in1 && n_in2) {
                    const float ev  = re[x + 1];
                    const float cap = eu*eu + ev*ev;
                    graph.addEdges(idx, idx + 1, cap, cap);
                }
            }

            // Down neighbour
            if (cy + 1 < ch) {
                const int ny   = y + 1;
                const int nidx = idx + cw;
                const cv::Vec4f* nr1 = f1.ptr<cv::Vec4f>(ny);
                const cv::Vec4f* nr2 = f2.ptr<cv::Vec4f>(ny);
                const bool n_in1 = nr1[x][3] > 0.5f;
                const bool n_in2 = nr2[x][3] > 0.5f;
                if (n_in1 && n_in2) {
                    const float ev  = err.ptr<float>(ny)[x];
                    const float cap = eu*eu + ev*ev;
                    graph.addEdges(idx, nidx, cap, cap);
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
