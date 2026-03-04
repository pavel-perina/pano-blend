#include "tiff_io.h"

#include <opencv2/imgcodecs.hpp>
#include <tiffio.h>
#include <cmath>
#include <format>
#include <stdexcept>
#include <print>

namespace tiffio {

// Register Pixar private tags so libtiff doesn't warn about them.
static void registerPixarTags(TIFF* tif) {
    // field_name must be non-const char* per the C struct definition.
    static char kFW[] = "PixarImageFullWidth";
    static char kFH[] = "PixarImageFullLength";
    static const TIFFFieldInfo info[] = {
        { 33300, 1, 1, TIFF_LONG, FIELD_CUSTOM, 1, 0, kFW },
        { 33301, 1, 1, TIFF_LONG, FIELD_CUSTOM, 1, 0, kFH },
    };
    TIFFMergeFieldInfo(tif, info, 2);
}

TiffImage readTiff(const std::string& path, int xoff, int yoff) {
    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif)
        throw std::runtime_error("Cannot open: " + path);

    registerPixarTags(tif);

    // --- Format check: only 8-bit RGB and RGBA are implemented ---
    uint16_t spp = 1, bps = 8, photo = PHOTOMETRIC_RGB;
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE,   &bps);
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC,     &photo);

    if (bps != 8) {
        TIFFClose(tif);
        throw std::runtime_error(std::format(
            "Not implemented: '{}' is {}-bit (only 8-bit supported)", path, bps));
    }
    if (photo != PHOTOMETRIC_RGB) {
        TIFFClose(tif);
        throw std::runtime_error(std::format(
            "Not implemented: '{}' photometric={} (only RGB/RGBA supported)",
            path, photo));
    }
    if (spp != 3 && spp != 4) {
        TIFFClose(tif);
        throw std::runtime_error(std::format(
            "Not implemented: '{}' has {} samples/pixel (only 3 or 4 supported)",
            path, spp));
    }

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,  &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);

    // --- Position tags ---
    float    tx = 0.0f, ty = 0.0f, xres = 1.0f, yres = 1.0f;
    uint32_t canvas_w = 0, canvas_h = 0;
    TIFFGetField(tif, TIFFTAG_XPOSITION,   &tx);
    TIFFGetField(tif, TIFFTAG_YPOSITION,   &ty);
    TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres);
    TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres);
    TIFFGetField(tif, 33300, &canvas_w);
    TIFFGetField(tif, 33301, &canvas_h);

    // --- Read pixels via TIFFReadRGBAImage ---
    // Always returns RGBA uint8, bottom-to-top row order.
    std::vector<uint32_t> raster(w * h);
    if (!TIFFReadRGBAImage(tif, w, h, raster.data(), /*stop_on_error=*/0)) {
        TIFFClose(tif);
        throw std::runtime_error("TIFFReadRGBAImage failed: " + path);
    }
    TIFFClose(tif);

    // Convert: RGBA uint8 bottom-to-top → CV_32FC4 BGRA float [0,1] top-to-bottom
    cv::Mat mat(static_cast<int>(h), static_cast<int>(w), CV_32FC4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t p = raster[(h - 1 - y) * w + x];  // flip vertical
            mat.at<cv::Vec4f>(y, x) = {
                TIFFGetB(p) / 255.0f,
                TIFFGetG(p) / 255.0f,
                TIFFGetR(p) / 255.0f,
                TIFFGetA(p) / 255.0f,
            };
        }
    }

    TiffImage result;
    result.mat      = std::move(mat);
    result.x        = (xoff >= 0) ? xoff : static_cast<int>(std::round(tx * xres));
    result.y        = (yoff >= 0) ? yoff : static_cast<int>(std::round(ty * yres));
    result.canvas_w = static_cast<int>(canvas_w);
    result.canvas_h = static_cast<int>(canvas_h);
    result.path     = path;
    return result;
}

cv::Size canvasSize(const std::vector<TiffImage>& images) {
    for (const auto& img : images) {
        if (img.canvas_w > 0 && img.canvas_h > 0)
            return { img.canvas_w, img.canvas_h };
    }
    int w = 0, h = 0;
    for (const auto& img : images) {
        w = std::max(w, img.x + img.mat.cols);
        h = std::max(h, img.y + img.mat.rows);
    }
    return { w, h };
}

cv::Mat placeOnCanvas(const TiffImage& img, cv::Size canvas) {
    cv::Mat result = cv::Mat::zeros(canvas, CV_32FC4);
    img.mat.copyTo(result(cv::Rect(img.x, img.y, img.mat.cols, img.mat.rows)));
    return result;
}

void writeTiff(const std::string& path, const cv::Mat& mat) {
    cv::Mat out;
    if (mat.depth() == CV_32F)
        mat.convertTo(out, CV_8UC4, 255.0);
    else
        out = mat;
    const std::vector<int> params = { cv::IMWRITE_TIFF_COMPRESSION, 1 };
    if (!cv::imwrite(path, out, params))
        throw std::runtime_error("Cannot write: " + path);
}

} // namespace tiffio
