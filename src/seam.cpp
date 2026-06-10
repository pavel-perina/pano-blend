#include "seam.h"
#include "colors.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/detail/gcgraph.hpp>
#include <cmath>
#include <print>

namespace seam {

void computeError(const cv::Mat& f1, const cv::Mat& f2, cv::Mat& err,
                  bool grayscale) {
    CV_Assert(f1.size() == f2.size() && f1.size() == err.size());
    CV_Assert(err.type() == CV_32FC1);

    cv::parallel_for_(cv::Range(0, f1.rows), [&](const cv::Range& range) {
        for (int y = range.start; y < range.end; ++y) {
            const cv::Vec4f* r1 = f1.ptr<cv::Vec4f>(y);
            const cv::Vec4f* r2 = f2.ptr<cv::Vec4f>(y);
            float*           re = err.ptr<float>(y);
            for (int x = 0; x < f1.cols; ++x) {
                if (r1[x][3] < 0.5f || r2[x][3] < 0.5f) {
                    re[x] = kNoOverlap;
                    continue;
                }
                if (grayscale) {
                    // R=G=B=gray internally, just compare one channel
                    re[x] = std::abs(r1[x][0] - r2[x][0]);
                } else {
                    // OKLab perceptual distance for color images
                    // OpenCV BGRA: [0]=B [1]=G [2]=R [3]=A
                    const auto lab1 = color::okLabFromRgb({r1[x][2], r1[x][1], r1[x][0]});
                    const auto lab2 = color::okLabFromRgb({r2[x][2], r2[x][1], r2[x][0]});
                    const float dL = lab1.L - lab2.L;
                    const float da = lab1.a - lab2.a;
                    const float db = lab1.b - lab2.b;
                    re[x] = std::sqrt(dL*dL + da*da + db*db);
                }
            }
        }
    });
}

// ---------------------------------------------------------------------------
// BK graph-cut over a crop of the overlap region.
//
// All inputs are crop-sized: a1/a2 are the alpha channels (CV_32FC1) of the
// two images, err the error map (CV_32FC1).  Only overlap pixels (both
// alphas > 0.5) get edges.  Boundary overlap pixels adjacent to single-image
// territory get kHard T-weights.  If |band| is non-empty, only pixels where
// band > 0 participate; pixels at the band boundary get kHard T-weights based
// on the coarse_mask (also crop-sized).
//
// Returns a crop-sized CV_8UC1 mask (0=img1, 255=img2); single-image pixels
// keep their own side.
// ---------------------------------------------------------------------------
static cv::Mat graphCut(const cv::Mat& a1, const cv::Mat& a2, const cv::Mat& err,
                        const cv::Mat& band, const cv::Mat& coarse_mask) {
    const int H = a1.rows;
    const int W = a1.cols;
    const bool use_band = !band.empty();

    constexpr float kHard = 1e6f;

    const int n_vtx = H * W;
    cv::detail::GCGraph<float> graph;
    graph.create(n_vtx, 2 * n_vtx);
    for (int i = 0; i < n_vtx; ++i) graph.addVtx();
    int n_edges = 0;

    for (int y = 0; y < H; ++y) {
        const float*   ra1 = a1.ptr<float>(y);
        const float*   ra2 = a2.ptr<float>(y);
        const float*   re  = err.ptr<float>(y);
        const uint8_t* rb  = use_band ? band.ptr<uint8_t>(y) : nullptr;
        const uint8_t* rc  = use_band ? coarse_mask.ptr<uint8_t>(y) : nullptr;
        const float*   ua1 = (y > 0)     ? a1.ptr<float>(y-1) : nullptr;
        const float*   ua2 = (y > 0)     ? a2.ptr<float>(y-1) : nullptr;
        const float*   da1 = (y+1 < H)   ? a1.ptr<float>(y+1) : nullptr;
        const float*   da2 = (y+1 < H)   ? a2.ptr<float>(y+1) : nullptr;

        for (int x = 0; x < W; ++x) {
            const int idx = y * W + x;
            const bool in1 = ra1[x] > 0.5f;
            const bool in2 = ra2[x] > 0.5f;

            if (!(in1 && in2)) continue;
            if (use_band && rb[x] == 0) continue;

            // --- T-weights ---
            float tw_src = 0.0f, tw_snk = 0.0f;

            // Adjacent to single-image territory → hard T-weight
            if ((x > 0   && ra1[x-1] > 0.5f && ra2[x-1] <= 0.5f) ||
                (x+1 < W && ra1[x+1] > 0.5f && ra2[x+1] <= 0.5f) ||
                (ua1 && ua1[x] > 0.5f && ua2[x] <= 0.5f) ||
                (da1 && da1[x] > 0.5f && da2[x] <= 0.5f))
                tw_src = kHard;

            if ((x > 0   && ra2[x-1] > 0.5f && ra1[x-1] <= 0.5f) ||
                (x+1 < W && ra2[x+1] > 0.5f && ra1[x+1] <= 0.5f) ||
                (ua2 && ua2[x] > 0.5f && ua1[x] <= 0.5f) ||
                (da2 && da2[x] > 0.5f && da1[x] <= 0.5f))
                tw_snk = kHard;

            // In band mode: pixels at the band edge get hard T-weight from coarse mask
            if (use_band) {
                bool at_band_edge = false;
                if (x > 0   && rb[x-1] == 0) at_band_edge = true;
                if (x+1 < W && rb[x+1] == 0) at_band_edge = true;
                if (y > 0   && band.ptr<uint8_t>(y-1)[x] == 0) at_band_edge = true;
                if (y+1 < H && band.ptr<uint8_t>(y+1)[x] == 0) at_band_edge = true;

                if (at_band_edge) {
                    if (rc[x] == 0) tw_src = kHard;  // coarse says img1 → source
                    else            tw_snk = kHard;  // coarse says img2 → sink
                }
            }

            if (tw_src > 0 || tw_snk > 0)
                graph.addTermWeights(idx, tw_src, tw_snk);

            // --- N-weight edges (quantised to integer scale) ---
            // Zero-capacity edges are no-ops for the min-cut, so skip them
            // (where the images are identical the seam placement is
            // irrelevant to the output anyway).
            const int du = std::min(255, static_cast<int>(re[x] * 255.0f));

            // Right neighbour
            if (x + 1 < W && ra1[x+1] > 0.5f && ra2[x+1] > 0.5f &&
                (!use_band || rb[x+1] != 0)) {
                const int dv = std::min(255, static_cast<int>(re[x+1] * 255.0f));
                if (du + dv > 0) {
                    const float cap = static_cast<float>(du*du + dv*dv);
                    graph.addEdges(idx, idx + 1, cap, cap);
                    ++n_edges;
                }
            }

            // Down neighbour
            if (y + 1 < H && da1[x] > 0.5f && da2[x] > 0.5f &&
                (!use_band || band.ptr<uint8_t>(y+1)[x] != 0)) {
                const int dv = std::min(255, static_cast<int>(err.ptr<float>(y+1)[x] * 255.0f));
                if (du + dv > 0) {
                    const float cap = static_cast<float>(du*du + dv*dv);
                    graph.addEdges(idx, idx + W, cap, cap);
                    ++n_edges;
                }
            }
        }
    }

    // GCGraph::maxFlow asserts on an edgeless graph (e.g. isolated overlap
    // pixels) — fall back to the coarse mask / img1 below in that case.
    const bool has_cut = n_edges > 0;
    if (has_cut)
        graph.maxFlow();

    // Build crop-sized mask.  For each pixel:
    //   - Both images present (overlap) → use graph cut result
    //   - Only img2 → mask=255; otherwise → mask=0
    cv::Mat mask(H, W, CV_8UC1);
    for (int y = 0; y < H; ++y) {
        const float* ra1 = a1.ptr<float>(y);
        const float* ra2 = a2.ptr<float>(y);
        uint8_t*     rm  = mask.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) {
            const bool in1 = ra1[x] > 0.5f;
            const bool in2 = ra2[x] > 0.5f;

            if (!(in1 && in2)) {
                rm[x] = (in2 && !in1) ? 255 : 0;
            } else if (use_band && band.ptr<uint8_t>(y)[x] == 0) {
                rm[x] = coarse_mask.ptr<uint8_t>(y)[x];
            } else if (!has_cut) {
                rm[x] = use_band ? coarse_mask.ptr<uint8_t>(y)[x] : 0;
            } else {
                rm[x] = graph.inSourceSegment(y * W + x) ? 0 : 255;
            }
        }
    }
    return mask;
}

// Expand a crop-local mask to the full canvas.  Single-image pixels keep
// their own side; overlap pixels (always inside |crop|) take the crop mask.
// With an empty crop there is no overlap and crop_mask is never read.
static cv::Mat assembleMask(const cv::Mat& f1, const cv::Mat& f2,
                            const cv::Rect& crop, const cv::Mat& crop_mask) {
    const int H = f1.rows;
    const int W = f1.cols;
    cv::Mat mask(H, W, CV_8UC1);
    for (int y = 0; y < H; ++y) {
        const cv::Vec4f* r1 = f1.ptr<cv::Vec4f>(y);
        const cv::Vec4f* r2 = f2.ptr<cv::Vec4f>(y);
        uint8_t*         rm = mask.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) {
            const bool in1 = r1[x][3] > 0.5f;
            const bool in2 = r2[x][3] > 0.5f;
            if (in1 && in2) {
                rm[x] = crop_mask.ptr<uint8_t>(y - crop.y)[x - crop.x];
            } else {
                rm[x] = (in2 && !in1) ? 255 : 0;
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
        // No pixel-level overlap (bounding boxes may still intersect, e.g.
        // warped images with transparent corners).  Return the coverage
        // split so each image keeps its own territory — an all-zero mask
        // here would hand image 2's pixels to image 1 and punch holes.
        return assembleMask(f1, f2, {}, {});
    }

    constexpr int kScale = 8;       // downsample factor for coarse pass
    constexpr int kBandRadius = 64; // half-width of refinement band (pixels)

    // Expand the crop to include single-image border pixels.  The full-res
    // graph only needs 1, but the coarse pass downsamples the crop by kScale,
    // so a kScale-wide ring is needed for the border to survive — it anchors
    // the hard T-weights; without it the coarse cut is unconstrained.
    ox0 = std::max(0, ox0 - kScale);
    oy0 = std::max(0, oy0 - kScale);
    ox1 = std::min(W, ox1 + kScale);
    oy1 = std::min(H, oy1 + kScale);

    const cv::Rect crop(ox0, oy0, ox1 - ox0, oy1 - oy0);
    const int cw = crop.width;
    const int ch = crop.height;
    std::println("  Overlap crop: {}x{} at ({},{}) [canvas {}x{}]", cw, ch, ox0, oy0, W, H);

    // Work crop-local from here on: the graph cut only needs the alpha
    // channels and the error map inside the crop.
    cv::Mat a1, a2;
    cv::extractChannel(f1(crop), a1, 3);
    cv::extractChannel(f2(crop), a2, 3);
    const cv::Mat errc = err(crop);

    // --- Coarse-to-fine seam finding ---
    //
    // 1. Coarse pass: downsample alphas+error, run BK on small graph.
    //    Even with deep BK trees, the graph is small → fast.
    // 2. Fine pass: build full-res graph only in a narrow band around
    //    the coarse seam.  Band edges get hard T-weights from coarse mask.
    //    Band is narrow → shallow trees → fast, no bias.

    // For small overlaps, skip coarse pass — run directly
    if (cw < kScale * 4 || ch < kScale * 4) {
        return assembleMask(f1, f2, crop, graphCut(a1, a2, errc, {}, {}));
    }

    // --- Coarse pass ---
    cv::Mat a1_small, a2_small, err_small;
    cv::resize(a1,   a1_small,  {}, 1.0 / kScale, 1.0 / kScale, cv::INTER_AREA);
    cv::resize(a2,   a2_small,  {}, 1.0 / kScale, 1.0 / kScale, cv::INTER_AREA);
    cv::resize(errc, err_small, {}, 1.0 / kScale, 1.0 / kScale, cv::INTER_AREA);

    std::println("  Coarse pass: {}x{} (1/{})", a1_small.cols, a1_small.rows, kScale);
    cv::Mat coarse_small = graphCut(a1_small, a2_small, err_small, {}, {});

    // Upscale coarse mask to crop resolution
    cv::Mat coarse;
    cv::resize(coarse_small, coarse, {cw, ch}, 0, 0, cv::INTER_NEAREST);

    // --- Build band mask around coarse seam ---
    // Mark seam pixels (where the mask changes between neighbours), then take
    // everything within kBandRadius of them via a distance transform — O(n),
    // unlike dilating with a (2r+1)² ellipse kernel.
    cv::Mat seam_edge(ch, cw, CV_8UC1, cv::Scalar(255));
    for (int y = 0; y < ch; ++y) {
        const uint8_t* rm = coarse.ptr<uint8_t>(y);
        uint8_t*       se = seam_edge.ptr<uint8_t>(y);
        for (int x = 0; x < cw; ++x) {
            const uint8_t v = rm[x];
            if ((x+1 < cw && rm[x+1] != v) ||
                (y+1 < ch && coarse.ptr<uint8_t>(y+1)[x] != v))
                se[x] = 0;  // seam pixel (distanceTransform measures to zeros)
        }
    }

    cv::Mat dist;
    cv::distanceTransform(seam_edge, dist, cv::DIST_L2, cv::DIST_MASK_5);
    cv::Mat band_mask = dist <= static_cast<float>(kBandRadius);

    // Count band pixels
    int band_pixels = cv::countNonZero(band_mask);
    std::println("  Fine band: {} pixels (vs {} overlap)", band_pixels, cw * ch);

    // Uniform coarse mask → no seam edge, nothing to refine
    if (band_pixels == 0) {
        return assembleMask(f1, f2, crop, coarse);
    }

    // --- Fine pass ---
    return assembleMask(f1, f2, crop, graphCut(a1, a2, errc, band_mask, coarse));
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
