#include <opencv2/imgcodecs.hpp>
#include <cmath>
#include <limits>
#include <print>

#include "colors.h"

int main() {
    cv::Mat img1 = cv::imread("test-data/p1.tif", cv::IMREAD_UNCHANGED);
    cv::Mat img2 = cv::imread("test-data/p2.tif", cv::IMREAD_UNCHANGED);

    if (img1.empty()) {
        std::println(stderr, "Cannot load test-data/p1.tif");
        return 1;
    }
    if (img2.empty()) {
        std::println(stderr, "Cannot load test-data/p2.tif");
        return 1;
    }

    if (img1.size() != img2.size() || img1.channels() != 4 || img2.channels() != 4) {
        std::println(stderr, "Images must be the same size and 4-channel RGBA");
        return 1;
    }

    // Convert BGRA uint8 → float [0,1]
    cv::Mat f1, f2;
    img1.convertTo(f1, CV_32FC4, 1.0 / 255.0);
    img2.convertTo(f2, CV_32FC4, 1.0 / 255.0);

    const int h = f1.rows;
    const int w = f1.cols;

    // Sentinel for pixels outside the overlap — large enough that no real
    // OKLab distance will exceed it, finite so downstream math stays safe.
    constexpr float kNoOverlap = 1e6f;

    // Per-pixel OKLab distance; full canvas size, positions match pixel coords
    cv::Mat err(h, w, CV_32FC1);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto p1 = f1.at<cv::Vec4f>(y, x);
            const auto p2 = f2.at<cv::Vec4f>(y, x);

            // OpenCV BGRA order: [0]=B [1]=G [2]=R [3]=A
            if (p1[3] < 0.5f || p2[3] < 0.5f) {
                err.at<float>(y, x) = kNoOverlap;
                continue;
            }

            const auto lab1 = color::okLabFromRgb({p1[2], p1[1], p1[0]});
            const auto lab2 = color::okLabFromRgb({p2[2], p2[1], p2[0]});

            const float dL = lab1.L - lab2.L;
            const float da = lab1.a - lab2.a;
            const float db = lab1.b - lab2.b;
            err.at<float>(y, x) = std::sqrt(dL*dL + da*da + db*db);
        }
    }

    const std::vector<int> tiff_params = { cv::IMWRITE_TIFF_COMPRESSION, 1 };
    if (!cv::imwrite("test-data/error.tif", err, tiff_params)) {
        std::println(stderr, "Failed to write test-data/error.tif");
        return 1;
    }

    double minVal, maxVal;
    cv::minMaxLoc(err, &minVal, &maxVal);
    std::println("OKLab error range in overlap: [{}, {}]", minVal, maxVal);

    return 0;
}
