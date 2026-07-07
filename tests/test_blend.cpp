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

    cv::Mat img1(H, W, CV_32FC4, cv::Scalar(0, 0, 0, 0));   // white, x < 300
    img1(cv::Rect(0, 0, 300, H)).setTo(cv::Scalar(v, v, v, 1.0f));

    cv::Mat img2(H, W, CV_32FC4, cv::Scalar(0, 0, 0, 0));   // white, x >= 100,
    img2(cv::Rect(100, 0, 300, H)).setTo(cv::Scalar(v, v, v, 1.0f));
    for (int y = 0; y < H; ++y)                              // transparent wedge
        for (int x = 100; x < W; ++x)
            if ((x - 100) + y < 120) img2.at<cv::Vec4f>(y, x) = {0, 0, 0, 0};

    // Label map consistent with coverage: image 2 wins where present and
    // x >= 150, image 1 wins elsewhere it is present.
    cv::Mat label(H, W, CV_16UC1, cv::Scalar(0));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool in1 = img1.at<cv::Vec4f>(y, x)[3] > 0.5f;
            const bool in2 = img2.at<cv::Vec4f>(y, x)[3] > 0.5f;
            if (in2 && (x >= 150 || !in1)) label.at<uint16_t>(y, x) = 2;
            else if (in1)                  label.at<uint16_t>(y, x) = 1;
        }

    const cv::Mat out = blend::multiBandBlend({img1, img2}, label);
    ASSERT_EQ(out.type(), CV_8UC4);

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
// composite. A 20x15 grid of tile images exercises labels beyond 255; the
// probed tile is interior, so pyramid weight support is complete and its
// colour survives intact.
TEST(MultiBandBlend, LabelsAbove255Survive) {
    const int cols = 20, rows = 15, tile = 16;   // N = 300
    const int W = cols * tile, H = rows * tile;
    const float v = 200.0f / 255.0f;

    std::vector<cv::Mat> images(cols * rows);
    cv::Mat label(H, W, CV_16UC1, cv::Scalar(0));
    for (int i = 0; i < cols * rows; ++i) {
        const cv::Rect r((i % cols) * tile, (i / cols) * tile, tile, tile);
        images[i] = cv::Mat(H, W, CV_32FC4, cv::Scalar(0, 0, 0, 0));
        images[i](r).setTo(cv::Scalar(v, v, v, 1.0f));
        label(r).setTo(cv::Scalar(i + 1));
    }

    const cv::Mat out = blend::multiBandBlend(images, label);
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
