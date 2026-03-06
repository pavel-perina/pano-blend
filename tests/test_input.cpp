#include "tiff_io.h"
#include <gtest/gtest.h>

// tiffio::readTiff returns CV_32FC4 BGRA float [0,1].
// Vec4f channels: [0]=B  [1]=G  [2]=R  [3]=A

static tiffio::TiffImage load_p1() {
    return tiffio::readTiff(TEST_DATA_DIR "/p1.tif");
}

static tiffio::TiffImage load_p2() {
    return tiffio::readTiff(TEST_DATA_DIR "/p2.tif");
}

// Helper: compare float channel to expected uint8 value (±1 for rounding)
static void expectChannel(float actual, int expected_u8) {
    int actual_u8 = static_cast<int>(std::round(actual * 255.0f));
    EXPECT_NEAR(actual_u8, expected_u8, 1);
}

TEST(InputP1, LoadsSuccessfully) {
    auto img = load_p1();
    ASSERT_FALSE(img.mat.empty());
    EXPECT_EQ(img.mat.cols,      405);
    EXPECT_EQ(img.mat.rows,      240);
    EXPECT_EQ(img.mat.channels(),  4);
    EXPECT_EQ(img.mat.depth(),   CV_32F);
}

TEST(InputP1, TopLeftPixelIsOpaque) {
    auto img = load_p1();
    ASSERT_FALSE(img.mat.empty());
    auto px = img.mat.at<cv::Vec4f>(0, 0);
    expectChannel(px[2], 15);   // R
    expectChannel(px[1], 18);   // G
    expectChannel(px[0], 23);   // B
    expectChannel(px[3], 255);  // A — fully opaque
}

TEST(InputP1, TopRightPixelIsTransparent) {
    auto img = load_p1();
    ASSERT_FALSE(img.mat.empty());
    auto px = img.mat.at<cv::Vec4f>(0, 404);
    expectChannel(px[3], 0);    // A — fully transparent
}

TEST(InputP2, LoadsSuccessfully) {
    auto img = load_p2();
    ASSERT_FALSE(img.mat.empty());
    EXPECT_EQ(img.mat.cols,      405);
    EXPECT_EQ(img.mat.rows,      240);
    EXPECT_EQ(img.mat.channels(),  4);
    EXPECT_EQ(img.mat.depth(),   CV_32F);
}

TEST(InputP2, TopLeftPixelIsTransparent) {
    auto img = load_p2();
    ASSERT_FALSE(img.mat.empty());
    auto px = img.mat.at<cv::Vec4f>(0, 0);
    expectChannel(px[3], 0);    // A — p2 starts at x=85, so x=0 is empty
}

TEST(InputP2, TopRightPixelIsOpaque) {
    auto img = load_p2();
    ASSERT_FALSE(img.mat.empty());
    auto px = img.mat.at<cv::Vec4f>(0, 404);
    expectChannel(px[2],  3);   // R
    expectChannel(px[1],  2);   // G
    expectChannel(px[0],  8);   // B
    expectChannel(px[3], 255);  // A — fully opaque
}
