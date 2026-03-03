#include <opencv2/imgcodecs.hpp>
#include <print>

#include "seam.h"

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

    const std::vector<int> tiff_params = { cv::IMWRITE_TIFF_COMPRESSION, 1 };

    const cv::Mat err = seam::computeError(f1, f2);
    if (!cv::imwrite("test-data/error.tif", err, tiff_params)) {
        std::println(stderr, "Failed to write test-data/error.tif");
        return 1;
    }

    const cv::Mat mask = seam::findSeam(f1, f2, err);
    if (!cv::imwrite("test-data/seam.tif", mask, tiff_params)) {
        std::println(stderr, "Failed to write test-data/seam.tif");
        return 1;
    }

    const cv::Mat viz = seam::visualizeSeam(f1, f2, err, mask);
    if (!cv::imwrite("test-data/seam_viz.tif", viz, tiff_params)) {
        std::println(stderr, "Failed to write test-data/seam_viz.tif");
        return 1;
    }

    double minErr, maxErr;
    cv::minMaxLoc(err, &minErr, &maxErr, nullptr, nullptr, err < 1.0f);
    std::println("OKLab error range in overlap: [{:.4f}, {:.4f}]", minErr, maxErr);
    std::println("Seam mask written to test-data/seam.tif");

    return 0;
}
