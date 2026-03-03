#include "blend.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/stitching/detail/blenders.hpp>

namespace blend {

cv::Mat multiBandBlend(const cv::Mat& f1, const cv::Mat& f2,
                       const cv::Mat& seam_mask, int num_bands) {
    const int h = f1.rows;
    const int w = f1.cols;

    // Split float BGRA into individual channels
    std::vector<cv::Mat> ch1, ch2;
    cv::split(f1, ch1);  // [B, G, R, A]
    cv::split(f2, ch2);

    // Presence masks: 255 where each image has opaque pixels
    const cv::Mat in1 = ch1[3] > 0.5f;
    const cv::Mat in2 = ch2[3] > 0.5f;

    // Territory masks: seam_mask==0 → image1 wins, seam_mask==255 → image2 wins
    // Both values are 0 or 255, so bitwise_and works as logical AND.
    cv::Mat mask1, mask2;
    cv::bitwise_and(seam_mask == 0, in1, mask1);
    cv::bitwise_and(seam_mask,      in2, mask2);

    // Convert float BGR [0,1] → CV_16SC3 [0,255] for MultiBandBlender
    cv::Mat bgr1, bgr2, bgr1_16s, bgr2_16s;
    cv::merge(std::vector<cv::Mat>{ch1[0], ch1[1], ch1[2]}, bgr1);
    cv::merge(std::vector<cv::Mat>{ch2[0], ch2[1], ch2[2]}, bgr2);
    bgr1.convertTo(bgr1_16s, CV_16SC3, 255.0);
    bgr2.convertTo(bgr2_16s, CV_16SC3, 255.0);

    cv::detail::MultiBandBlender blender(/*try_gpu=*/false, num_bands);
    blender.prepare(cv::Rect(0, 0, w, h));
    blender.feed(bgr1_16s, mask1, cv::Point(0, 0));
    blender.feed(bgr2_16s, mask2, cv::Point(0, 0));

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
