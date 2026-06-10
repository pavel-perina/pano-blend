#include "blend.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/stitching/detail/blenders.hpp>

namespace blend {

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

        // Convert float BGRA [0,1] → CV_16SC3 [0,255]
        cv::Mat bgr, bgr_16s;
        cv::cvtColor(images[i](roi), bgr, cv::COLOR_BGRA2BGR);
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
