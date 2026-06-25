#include "blend.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/stitching/detail/blenders.hpp>

#include <vector>

namespace blend {

// Fill non-present (transparent) pixels of a fed crop with their nearest
// present colour.  MultiBandBlender builds the image's Laplacian pyramid over
// the whole fed rect — transparent pixels arrive as black (0,0,0) and, at
// coarse pyramid levels, that black bleeds across the mask boundary and
// darkens the valid pixels beside it (the dark halo where a seam meets an
// image's transparent edge).  These hole pixels are masked out of the blend
// weight, so their value only needs to match the boundary instead of black.
static void fillHoles(cv::Mat& bgr, const cv::Mat& present) {
    cv::Mat invalid;
    cv::bitwise_not(present, invalid);              // 255 at holes, 0 at valid
    if (cv::countNonZero(invalid) == 0) return;

    // labels[hole] = label of the nearest valid (zero) pixel; each valid
    // pixel gets its own label, so label → colour is a clean lookup.
    cv::Mat dist, labels;
    cv::distanceTransform(invalid, dist, labels, cv::DIST_L2, 3,
                          cv::DIST_LABEL_PIXEL);

    double max_label = 0.0;
    cv::minMaxLoc(labels, nullptr, &max_label);
    std::vector<cv::Vec3f> colour(static_cast<size_t>(max_label) + 1);

    for (int y = 0; y < bgr.rows; ++y) {
        const int32_t*   lab = labels.ptr<int32_t>(y);
        const uint8_t*   pr  = present.ptr<uint8_t>(y);
        const cv::Vec3f* px  = bgr.ptr<cv::Vec3f>(y);
        for (int x = 0; x < bgr.cols; ++x)
            if (pr[x]) colour[lab[x]] = px[x];
    }
    for (int y = 0; y < bgr.rows; ++y) {
        const int32_t* lab = labels.ptr<int32_t>(y);
        const uint8_t* pr  = present.ptr<uint8_t>(y);
        cv::Vec3f*     px  = bgr.ptr<cv::Vec3f>(y);
        for (int x = 0; x < bgr.cols; ++x)
            if (!pr[x]) px[x] = colour[lab[x]];
    }
}

cv::Mat multiBandBlend(const std::vector<cv::Mat>& images,
                       const cv::Mat& label_map, int num_bands) {
    const int N = static_cast<int>(images.size());
    const int h = images[0].rows;
    const int w = images[0].cols;

    cv::detail::MultiBandBlender blender(/*try_gpu=*/false, num_bands);
    blender.prepare(cv::Rect(0, 0, w, h));

    for (int i = 0; i < N; ++i) {
        // Territory mask: 255 where this image (1-based index i+1) wins
        cv::Mat territory;
        cv::compare(label_map, i + 1, territory, cv::CMP_EQ);

        // Presence mask: where this image has opaque pixels
        cv::Mat alpha;
        cv::extractChannel(images[i], alpha, 3);
        cv::Mat present = alpha > 0.5f;

        // Final mask: image must be present AND own the territory
        cv::Mat mask;
        cv::bitwise_and(territory, present, mask);

        // Feed only the image's bounding rect — each feed builds a pyramid
        // over the fed rect, so full-canvas feeds would cost N× canvas.
        const cv::Rect roi = cv::boundingRect(present);
        if (roi.empty()) continue;

        // Convert float BGRA [0,1] → CV_16SC3 [0,255], filling transparent
        // pixels with the nearest valid colour first (no black-halo bleed).
        cv::Mat bgr, bgr_16s;
        cv::cvtColor(images[i](roi), bgr, cv::COLOR_BGRA2BGR);
        fillHoles(bgr, present(roi));
        bgr.convertTo(bgr_16s, CV_16SC3, 255.0);

        blender.feed(bgr_16s, mask(roi), roi.tl());
    }

    cv::Mat dst_bgr, dst_mask;
    blender.blend(dst_bgr, dst_mask);  // CV_16SC3 + CV_8UC1

    // Convert back to CV_8UC3, then re-attach alpha
    cv::Mat dst_8u;
    dst_bgr.convertTo(dst_8u, CV_8U);

    std::vector<cv::Mat> chs;
    cv::split(dst_8u, chs);
    chs.push_back(dst_mask);

    cv::Mat out;
    cv::merge(chs, out);
    return out;
}

} // namespace blend
