// Converts sample files from smartblend to test files, which were compressed using
// ```sh
// convert out2.tiff -compress zip p2.tif
// convert out1.tiff -compress zip p1.tif
// ```
// and moved to test_data. It also fixes some missing tags from OpenCV

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>

int main()
{
    // Hardcoded layout:
    //   canvas: 405 x 240  (85 + 320)
    //   p1 (319x240) placed at x=0
    //   p2 (320x240) placed at x=85
    constexpr int canvas_w = 405;
    constexpr int canvas_h = 240;
    constexpr int p2_off_x = 85;

    cv::Mat p1 = cv::imread("/mnt/c/dev-c/blend/smartblend/p1.jpg");
    cv::Mat p2 = cv::imread("/mnt/c/dev-c/blend/smartblend/p2.jpg");

    if (p1.empty()) { std::cerr << "Cannot load p1.jpg\n"; return 1; }
    if (p2.empty()) { std::cerr << "Cannot load p2.jpg\n"; return 1; }

    std::cout << "p1: " << p1.cols << "x" << p1.rows << "\n";
    std::cout << "p2: " << p2.cols << "x" << p2.rows << "\n";

    // Two fully-transparent BGRA canvases (alpha = 0 in empty areas)
    cv::Mat canvas1(canvas_h, canvas_w, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    cv::Mat canvas2(canvas_h, canvas_w, CV_8UC4, cv::Scalar(0, 0, 0, 0));

    // BGR -> BGRA (alpha set to 255 by OpenCV)
    cv::Mat p1_bgra, p2_bgra;
    cv::cvtColor(p1, p1_bgra, cv::COLOR_BGR2BGRA);
    cv::cvtColor(p2, p2_bgra, cv::COLOR_BGR2BGRA);

    p1_bgra.copyTo(canvas1(cv::Rect(0,        0, p1.cols, p1.rows)));
    p2_bgra.copyTo(canvas2(cv::Rect(p2_off_x, 0, p2.cols, p2.rows)));

    // Pass BGRA directly — OpenCV's TIFF writer swaps B/R internally,
    // producing a correct RGBA file. An extra manual swap would double-flip.
    const std::vector<int> tiff_params = {
        cv::IMWRITE_TIFF_COMPRESSION, 1   // 1 = no compression (COMPRESSION_NONE)
    };

    if (!cv::imwrite("out1.tiff", canvas1, tiff_params))
        { std::cerr << "Failed to write out1.tiff\n"; return 1; }
    if (!cv::imwrite("out2.tiff", canvas2, tiff_params))
        { std::cerr << "Failed to write out2.tiff\n"; return 1; }

    std::cout << "Wrote out1.tiff and out2.tiff\n";
    return 0;
}

