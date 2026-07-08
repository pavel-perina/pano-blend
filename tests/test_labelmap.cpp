#include "labelmap.h"
#include "seam.h"

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <cmath>
#include <vector>

// Unit tests for the sequential-accumulate label map. All data is synthetic
// and in-memory; the key regression is that with two images the accumulate
// reproduces today's pairwise graph-cut exactly.

namespace {

// A canvas-sized CV_32FC4 image, opaque inside |r|, with a deterministic
// analytic pattern (no RNG) so seams land somewhere non-trivial.
cv::Mat makeImage(cv::Size canvas, cv::Rect r, float phase) {
    cv::Mat img(canvas, CV_32FC4, cv::Scalar(0, 0, 0, 0));
    for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x) {
            const float v = 0.5f + 0.35f * std::sin(0.05f * x + phase)
                                         * std::cos(0.07f * y - phase);
            img.at<cv::Vec4f>(y, x) = {v, v, v, 1.0f};
        }
    return img;
}

bool isPermutation(const std::vector<int>& order, size_t n) {
    if (order.size() != n) return false;
    std::vector<bool> seen(n, false);
    for (int i : order) {
        if (i < 0 || i >= static_cast<int>(n) || seen[i]) return false;
        seen[i] = true;
    }
    return true;
}

} // namespace

// A horizontal strip of five equal, equally-overlapping boxes: the root is
// the middle one, growth spreads outward, ties resolve to the lowest index.
TEST(PlacementOrder, StripGrowsFromCenter) {
    std::vector<cv::Rect> rects;
    for (int i = 0; i < 5; ++i) rects.emplace_back(i * 80, 0, 100, 100);

    const std::vector<int> order = labelmap::placementOrder(rects);
    EXPECT_EQ(order, (std::vector<int>{2, 1, 0, 3, 4}));
}

// The neighbour sharing more area is placed first even with a higher index.
TEST(PlacementOrder, LargerOverlapWinsOverLowerIndex) {
    const std::vector<cv::Rect> rects = {
        {0, 0, 120, 100},     // overlaps root by 20 px
        {100, 0, 100, 100},   // root (most central)
        {160, 0, 120, 100},   // overlaps root by 40 px
    };
    const std::vector<int> order = labelmap::placementOrder(rects);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 0}));
}

// Disconnected components must still yield a full permutation (growth
// restarts at the most central remaining image).
TEST(PlacementOrder, DisconnectedComponentsCovered) {
    const std::vector<cv::Rect> rects = {
        {0, 0, 100, 100}, {80, 0, 100, 100},        // cluster A
        {1000, 0, 100, 100}, {1080, 0, 100, 100},   // cluster B, far away
    };
    const std::vector<int> order = labelmap::placementOrder(rects);
    EXPECT_TRUE(isPermutation(order, rects.size()));
}

// The bridge to the previous implementation: with exactly two images the
// sequential accumulate is one cut of image 2 against image 1, so the label
// map must equal the plain pairwise seam applied to the coverage split.
TEST(Accumulate, TwoImagesMatchPairwiseCut) {
    const cv::Size canvas(300, 200);
    const cv::Rect r1(0, 0, 180, 200), r2(120, 0, 180, 200);
    const cv::Mat img1 = makeImage(canvas, r1, 0.0f);
    const cv::Mat img2 = makeImage(canvas, r2, 0.6f);

    // Expected: today's pairwise path, spelled out.
    cv::Mat err(canvas, CV_32FC1, cv::Scalar(seam::kNoOverlap));
    seam::computeError(img1, img2, err);
    const cv::Mat mask = seam::findSeam(img1, img2, err);
    cv::Mat expected(canvas, CV_16UC1, cv::Scalar(0));
    for (int y = 0; y < canvas.height; ++y)
        for (int x = 0; x < canvas.width; ++x) {
            const bool in1 = img1.at<cv::Vec4f>(y, x)[3] > 0.5f;
            const bool in2 = img2.at<cv::Vec4f>(y, x)[3] > 0.5f;
            if (in1 && in2)
                expected.at<uint16_t>(y, x) = mask.at<uint8_t>(y, x) ? 2 : 1;
            else if (in1) expected.at<uint16_t>(y, x) = 1;
            else if (in2) expected.at<uint16_t>(y, x) = 2;
        }

    // accumulate takes crops: hand it views of the placed images.
    const cv::Mat label =
        labelmap::accumulate({img1(r1), img2(r2)}, {r1, r2}, canvas, {0, 1});
    EXPECT_EQ(cv::countNonZero(label != expected), 0);
}

// Coherence at a 3-image overlap: every covered pixel is owned, and only by
// an image that actually has content there — the invariants the old all-pairs
// merge could not guarantee.
TEST(Accumulate, TripleOverlapInvariantsHold) {
    const cv::Size canvas(260, 240);
    const std::vector<cv::Rect> rects = {
        {0, 0, 140, 140}, {100, 0, 140, 140}, {50, 80, 140, 140}};
    std::vector<cv::Mat> images, crops;
    for (size_t i = 0; i < rects.size(); ++i) {
        images.push_back(makeImage(canvas, rects[i], 0.4f * static_cast<float>(i)));
        crops.push_back(images.back()(rects[i]));
    }

    const std::vector<int> order = labelmap::placementOrder(rects);
    ASSERT_TRUE(isPermutation(order, rects.size()));
    const cv::Mat label = labelmap::accumulate(crops, rects, canvas, order);

    for (int y = 0; y < canvas.height; ++y)
        for (int x = 0; x < canvas.width; ++x) {
            bool covered = false;
            for (const auto& img : images)
                covered = covered || img.at<cv::Vec4f>(y, x)[3] > 0.5f;
            const uint16_t l = label.at<uint16_t>(y, x);
            ASSERT_EQ(l != 0, covered) << "at (" << x << "," << y << ")";
            if (l != 0)
                ASSERT_GT(images[l - 1].at<cv::Vec4f>(y, x)[3], 0.5f)
                    << "label " << l << " owns a pixel image " << l
                    << " does not cover, at (" << x << "," << y << ")";
        }
}
