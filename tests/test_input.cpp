#include <opencv2/imgcodecs.hpp>
#include <gtest/gtest.h>

// OpenCV loads images in BGRA order regardless of file format.
// Vec4b channels: [0]=B  [1]=G  [2]=R  [3]=A

static cv::Mat load_p1()
{
    return cv::imread(TEST_DATA_DIR "/p1.tif", cv::IMREAD_UNCHANGED);
}

static cv::Mat load_p2()
{
    return cv::imread(TEST_DATA_DIR "/p2.tif", cv::IMREAD_UNCHANGED);
}

TEST(InputP1, LoadsSuccessfully)
{
    cv::Mat img = load_p1();
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(img.cols,     405);
    EXPECT_EQ(img.rows,     240);
    EXPECT_EQ(img.channels(), 4);
}

TEST(InputP1, TopLeftPixelIsOpaque)
{
    cv::Mat img = load_p1();
    ASSERT_FALSE(img.empty());
    auto px = img.at<cv::Vec4b>(0, 0);
    EXPECT_EQ(px[2], 15);   // R
    EXPECT_EQ(px[1], 18);   // G
    EXPECT_EQ(px[0], 23);   // B
    EXPECT_EQ(px[3], 255);  // A — fully opaque
}

TEST(InputP1, TopRightPixelIsTransparent)
{
    cv::Mat img = load_p1();
    ASSERT_FALSE(img.empty());
    auto px = img.at<cv::Vec4b>(0, 404);
    EXPECT_EQ(px[3], 0);    // A — fully transparent
}

TEST(InputP2, LoadsSuccessfully)
{
    cv::Mat img = load_p2();
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(img.cols,      405);
    EXPECT_EQ(img.rows,      240);
    EXPECT_EQ(img.channels(),  4);
}

TEST(InputP2, TopLeftPixelIsTransparent)
{
    cv::Mat img = load_p2();
    ASSERT_FALSE(img.empty());
    auto px = img.at<cv::Vec4b>(0, 0);
    EXPECT_EQ(px[3], 0);    // A — p2 starts at x=85, so x=0 is empty
}

TEST(InputP2, TopRightPixelIsOpaque)
{
    cv::Mat img = load_p2();
    ASSERT_FALSE(img.empty());
    auto px = img.at<cv::Vec4b>(0, 404);
    EXPECT_EQ(px[2],  3);   // R
    EXPECT_EQ(px[1],  2);   // G
    EXPECT_EQ(px[0],  8);   // B
    EXPECT_EQ(px[3], 255);  // A — fully opaque
}
