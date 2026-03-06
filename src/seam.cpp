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

// ---------------------------------------------------------------------------
// BK graph-cut on a rectangular region of the error map.
//
// Builds a graph over pixels in the crop [ox0,oy0)..[ox1,oy1) of the canvas.
// Only overlap pixels (both f1 and f2 have alpha > 0.5) get edges.
// Boundary overlap pixels adjacent to single-image territory get kHard
// T-weights.  If |band| is non-empty, only pixels where band > 0 participate;
// pixels at the band boundary get kHard T-weights based on the coarse_mask.
//
// Returns a CV_8UC1 mask over the full canvas (0=img1, 255=img2).
// ---------------------------------------------------------------------------
static cv::Mat graphCut(const cv::Mat& f1, const cv::Mat& f2, const cv::Mat& err,
                        int ox0, int oy0, int ox1, int oy1,
                        const cv::Mat& band, const cv::Mat& coarse_mask) {
    const int H = f1.rows;
    const int W = f1.cols;
    const int cw = ox1 - ox0;
    const int ch = oy1 - oy0;
    const bool use_band = !band.empty();

    constexpr float kHard = 1e6f;

    const int n_vtx = ch * cw;
    cv::detail::GCGraph<float> graph;
    graph.create(n_vtx, 2 * n_vtx);
    for (int i = 0; i < n_vtx; ++i) graph.addVtx();

    for (int cy = 0; cy < ch; ++cy) {
        const int y = cy + oy0;
        const cv::Vec4f* r1 = f1.ptr<cv::Vec4f>(y);
        const cv::Vec4f* r2 = f2.ptr<cv::Vec4f>(y);
        const float*     re = err.ptr<float>(y);
        const uint8_t*   rb = use_band ? band.ptr<uint8_t>(cy) : nullptr;
        const uint8_t*   rc = use_band ? coarse_mask.ptr<uint8_t>(y) : nullptr;

        for (int cx = 0; cx < cw; ++cx) {
            const int x   = cx + ox0;
            const int idx = cy * cw + cx;
            const bool in1 = r1[x][3] > 0.5f;
            const bool in2 = r2[x][3] > 0.5f;

            if (!(in1 && in2)) continue;
            if (use_band && rb[cx] == 0) continue;

            // --- T-weights ---
            float tw_src = 0.0f, tw_snk = 0.0f;

            // Adjacent to single-image territory → hard T-weight
            if ((x > 0   && r1[x-1][3] > 0.5f && r2[x-1][3] <= 0.5f) ||
                (x+1 < W && r1[x+1][3] > 0.5f && r2[x+1][3] <= 0.5f) ||
                (y > 0   && f1.ptr<cv::Vec4f>(y-1)[x][3] > 0.5f && f2.ptr<cv::Vec4f>(y-1)[x][3] <= 0.5f) ||
                (y+1 < H && f1.ptr<cv::Vec4f>(y+1)[x][3] > 0.5f && f2.ptr<cv::Vec4f>(y+1)[x][3] <= 0.5f))
                tw_src = kHard;

            if ((x > 0   && r2[x-1][3] > 0.5f && r1[x-1][3] <= 0.5f) ||
                (x+1 < W && r2[x+1][3] > 0.5f && r1[x+1][3] <= 0.5f) ||
                (y > 0   && f2.ptr<cv::Vec4f>(y-1)[x][3] > 0.5f && f1.ptr<cv::Vec4f>(y-1)[x][3] <= 0.5f) ||
                (y+1 < H && f2.ptr<cv::Vec4f>(y+1)[x][3] > 0.5f && f1.ptr<cv::Vec4f>(y+1)[x][3] <= 0.5f))
                tw_snk = kHard;

            // In band mode: pixels at the band edge get hard T-weight from coarse mask
            if (use_band) {
                bool at_band_edge = false;
                if (cx > 0    && rb[cx-1] == 0) at_band_edge = true;
                if (cx+1 < cw && rb[cx+1] == 0) at_band_edge = true;
                if (cy > 0    && band.ptr<uint8_t>(cy-1)[cx] == 0) at_band_edge = true;
                if (cy+1 < ch && band.ptr<uint8_t>(cy+1)[cx] == 0) at_band_edge = true;

                if (at_band_edge) {
                    if (rc[x] == 0) tw_src = kHard;  // coarse says img1 → source
                    else            tw_snk = kHard;  // coarse says img2 → sink
                }
            }

            if (tw_src > 0 || tw_snk > 0)
                graph.addTermWeights(idx, tw_src, tw_snk);

            // --- N-weight edges (quantised to integer scale) ---
            const int du = std::min(255, static_cast<int>(re[x] * 255.0f));

            // Right neighbour
            if (cx + 1 < cw && r1[x+1][3] > 0.5f && r2[x+1][3] > 0.5f &&
                (!use_band || rb[cx+1] != 0)) {
                const int dv = std::min(255, static_cast<int>(re[x+1] * 255.0f));
                const float cap = static_cast<float>(du*du + dv*dv);
                graph.addEdges(idx, idx + 1, cap, cap);
            }

            // Down neighbour
            if (cy + 1 < ch) {
                const int ny = y + 1;
                const cv::Vec4f* nr1 = f1.ptr<cv::Vec4f>(ny);
                const cv::Vec4f* nr2 = f2.ptr<cv::Vec4f>(ny);
                const uint8_t* nrb = use_band ? band.ptr<uint8_t>(cy+1) : nullptr;
                if (nr1[x][3] > 0.5f && nr2[x][3] > 0.5f &&
                    (!use_band || nrb[cx] != 0)) {
                    const int dv = std::min(255, static_cast<int>(err.ptr<float>(ny)[x] * 255.0f));
                    const float cap = static_cast<float>(du*du + dv*dv);
                    graph.addEdges(idx, cy * cw + cw + cx, cap, cap);
                }
            }
        }
    }

    graph.maxFlow();

    // Build full-size mask.  For each pixel:
    //   - Both images present (overlap) → use graph cut result
    //   - Only img1 → mask=0 (use img1)
    //   - Only img2 → mask=255 (use img2)
    //   - Neither   → mask=0 (transparent either way)
    cv::Mat mask(H, W, CV_8UC1);
    for (int y = 0; y < H; ++y) {
        const cv::Vec4f* r1 = f1.ptr<cv::Vec4f>(y);
        const cv::Vec4f* r2 = f2.ptr<cv::Vec4f>(y);
        uint8_t*         rm = mask.ptr<uint8_t>(y);
        const bool in_crop_y = (y >= oy0 && y < oy1);

        for (int x = 0; x < W; ++x) {
            const bool in1 = r1[x][3] > 0.5f;
            const bool in2 = r2[x][3] > 0.5f;

            if (!(in1 && in2)) {
                // Single-image or empty pixel — no graph cut needed
                rm[x] = (in2 && !in1) ? 255 : 0;
            } else if (in_crop_y && x >= ox0 && x < ox1) {
                const int cy = y - oy0, cx = x - ox0;
                if (use_band && band.ptr<uint8_t>(cy)[cx] == 0) {
                    rm[x] = coarse_mask.ptr<uint8_t>(y)[x];
                } else {
                    rm[x] = graph.inSourceSegment(cy * cw + cx) ? 0 : 255;
                }
            } else {
                // Overlap pixel outside crop (shouldn't happen, but safe fallback)
                rm[x] = 0;
            }
        }
    }
    return mask;
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

    // --- Coarse-to-fine seam finding ---
    //
    // 1. Coarse pass: downsample images+error, run BK on small graph.
    //    Even with deep BK trees, the graph is small → fast.
    // 2. Fine pass: build full-res graph only in a narrow band around
    //    the coarse seam.  Band edges get hard T-weights from coarse mask.
    //    Band is narrow → shallow trees → fast, no bias.

    constexpr int kScale = 8;       // downsample factor for coarse pass
    constexpr int kBandRadius = 64; // half-width of refinement band (pixels)

    // For small overlaps, skip coarse pass — run directly
    if (cw < kScale * 4 || ch < kScale * 4) {
        return graphCut(f1, f2, err, ox0, oy0, ox1, oy1, {}, {});
    }

    // --- Coarse pass ---
    cv::Mat f1_small, f2_small, err_small;
    cv::resize(f1, f1_small, {}, 1.0 / kScale, 1.0 / kScale, cv::INTER_AREA);
    cv::resize(f2, f2_small, {}, 1.0 / kScale, 1.0 / kScale, cv::INTER_AREA);
    cv::resize(err, err_small, {}, 1.0 / kScale, 1.0 / kScale, cv::INTER_AREA);

    const int sH = f1_small.rows, sW = f1_small.cols;
    const int sox0 = ox0 / kScale, soy0 = oy0 / kScale;
    const int sox1 = std::min(sW, ox1 / kScale + 1);
    const int soy1 = std::min(sH, oy1 / kScale + 1);

    std::println("  Coarse pass: {}x{} (1/{})", sox1-sox0, soy1-soy0, kScale);
    cv::Mat coarse_mask = graphCut(f1_small, f2_small, err_small,
                                   sox0, soy0, sox1, soy1, {}, {});

    // Upscale coarse mask to full resolution
    cv::Mat coarse_full;
    cv::resize(coarse_mask, coarse_full, {W, H}, 0, 0, cv::INTER_NEAREST);

    // --- Build band mask around coarse seam ---
    // Find seam pixels (where mask changes between neighbours) and dilate
    cv::Mat seam_edge(ch, cw, CV_8UC1, cv::Scalar(0));
    for (int cy = 0; cy < ch; ++cy) {
        const int y = cy + oy0;
        const uint8_t* rm = coarse_full.ptr<uint8_t>(y);
        uint8_t*       se = seam_edge.ptr<uint8_t>(cy);
        for (int cx = 0; cx < cw; ++cx) {
            const int x = cx + ox0;
            const uint8_t v = rm[x];
            if ((cx+1 < cw && rm[x+1] != v) ||
                (cy+1 < ch && coarse_full.ptr<uint8_t>(y+1)[x] != v))
                se[cx] = 255;
        }
    }

    cv::Mat band_mask;
    const int diam = 2 * kBandRadius + 1;
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {diam, diam});
    cv::dilate(seam_edge, band_mask, kernel);

    // Count band pixels
    int band_pixels = cv::countNonZero(band_mask);
    std::println("  Fine band: {} pixels (vs {} overlap)", band_pixels, cw * ch);

    // --- Fine pass ---
    cv::Mat fine_mask = graphCut(f1, f2, err, ox0, oy0, ox1, oy1,
                                 band_mask, coarse_full);
    return fine_mask;
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
