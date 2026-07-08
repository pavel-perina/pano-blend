#include "blend.h"
#include <gtest/gtest.h>
#include <opencv2/core.hpp>

// Synthetic-data regression tests for blend::multiBandBlend. Everything is
// generated in-memory at small sizes — no test images needed.

// Dark-halo regression (fillHoles): a white pair where one image has a
// transparent wedge inside its bounding rect. Transparent pixels enter the
// Laplacian pyramid as black; without nearest-colour hole filling that black
// bleeds across the mask boundary and darkens opaque output near the
// seam/edge junction (observed 254 -> ~197 before the fix).
TEST(MultiBandBlend, NoDarkHaloAtTransparentEdge) {
    const int   W = 400, H = 240;
    const float v = 254.0f / 255.0f;

    // Crops + placement: img1 covers x < 300, img2 covers x >= 100 with a
    // transparent wedge at its top-left corner (canvas coordinates).
    const cv::Rect r1(0, 0, 300, H), r2(100, 0, 300, H);
    cv::Mat img1(r1.size(), CV_32FC4, cv::Scalar(v, v, v, 1.0f));
    cv::Mat img2(r2.size(), CV_32FC4, cv::Scalar(v, v, v, 1.0f));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < r2.width; ++x)
            if (x + y < 120) img2.at<cv::Vec4f>(y, x) = {0, 0, 0, 0};

    // Label map consistent with coverage: image 2 wins where present and
    // x >= 150, image 1 wins elsewhere it is present.
    auto in1 = [&](int x, int y) { return r1.contains(cv::Point{x, y}); };
    auto in2 = [&](int x, int y) {
        return r2.contains(cv::Point{x, y}) && !((x - r2.x) + y < 120);
    };
    cv::Mat label(H, W, CV_16UC1, cv::Scalar(0));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (in2(x, y) && (x >= 150 || !in1(x, y))) label.at<uint16_t>(y, x) = 2;
            else if (in1(x, y))                        label.at<uint16_t>(y, x) = 1;
        }

    const cv::Mat out = blend::multiBandBlend({img1, img2}, {r1, r2}, label);
    ASSERT_EQ(out.type(), CV_8UC4);
    ASSERT_EQ(out.size(), label.size());

    int min_opaque = 255, dark = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const cv::Vec4b p = out.at<cv::Vec4b>(y, x);
            if (p[3] == 0) continue;
            for (int c = 0; c < 3; ++c) {
                min_opaque = std::min<int>(min_opaque, p[c]);
                if (p[c] < 250) ++dark;
            }
        }
    EXPECT_EQ(dark, 0) << "darkest opaque value: " << min_opaque;
}

// 16-bit label-map regression: with a uint8 label map, image index 256+
// wrapped (label 257 -> 1) and its territory silently vanished from the
// composite. A 20x15 grid of tile crops exercises labels beyond 255; the
// probed tile is interior, so pyramid weight support is complete and its
// colour survives intact.
TEST(MultiBandBlend, LabelsAbove255Survive) {
    const int cols = 20, rows = 15, tile = 16;   // N = 300
    const int W = cols * tile, H = rows * tile;
    const float v = 200.0f / 255.0f;

    std::vector<cv::Mat>  crops(cols * rows);
    std::vector<cv::Rect> rects(cols * rows);
    cv::Mat label(H, W, CV_16UC1, cv::Scalar(0));
    for (int i = 0; i < cols * rows; ++i) {
        rects[i] = cv::Rect((i % cols) * tile, (i / cols) * tile, tile, tile);
        crops[i] = cv::Mat(tile, tile, CV_32FC4, cv::Scalar(v, v, v, 1.0f));
        label(rects[i]).setTo(cv::Scalar(i + 1));
    }

    const cv::Mat out = blend::multiBandBlend(crops, rects, label);
    ASSERT_EQ(out.type(), CV_8UC4);

    // image index 256 (label 257) is the first one a uint8 map lost;
    // grid position (row 12, col 16) — interior.
    const int cx = (256 % cols) * tile + tile / 2;
    const int cy = (256 / cols) * tile + tile / 2;
    const cv::Vec4b p = out.at<cv::Vec4b>(cy, cx);
    EXPECT_EQ(p[3], 255) << "territory of image 257 missing from composite";
    // Failure mode under test is catastrophic (transparent/black pixel, ~0);
    // MultiBandBlender itself loses ~2% on small synthetic feeds, so the
    // colour check is a wide-tolerance sanity bound, not an accuracy claim.
    EXPECT_NEAR(p[0], 200, 20);
    EXPECT_NEAR(p[1], 200, 20);
    EXPECT_NEAR(p[2], 200, 20);
}
