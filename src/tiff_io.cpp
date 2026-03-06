#include "tiff_io.h"

#include <opencv2/imgproc.hpp>
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

    uint32_t w = 0, h = 0;
    uint16_t spp = 1, bps = 8, photo = PHOTOMETRIC_MINISBLACK;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,      &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH,     &h);
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE,   &bps);
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC,     &photo);

    if (bps != 8 && bps != 16) {
        TIFFClose(tif);
        throw std::runtime_error(std::format(
            "'{}': {}-bit not supported (only 8 and 16)", path, bps));
    }

    const bool is_gray = (photo == PHOTOMETRIC_MINISBLACK || photo == PHOTOMETRIC_MINISWHITE);
    const bool is_rgb  = (photo == PHOTOMETRIC_RGB);
    if (!is_gray && !is_rgb) {
        TIFFClose(tif);
        throw std::runtime_error(std::format(
            "'{}': photometric {} not supported", path, photo));
    }

    // Determine if there's an alpha channel
    const bool has_alpha = (is_gray && spp >= 2) || (is_rgb && spp >= 4);
    const float scale = (bps == 16) ? 1.0f / 65535.0f : 1.0f / 255.0f;

    // Position tags
    float    tx = 0.0f, ty = 0.0f, xres = 1.0f, yres = 1.0f;
    uint32_t canvas_w = 0, canvas_h = 0;
    TIFFGetField(tif, TIFFTAG_XPOSITION,   &tx);
    TIFFGetField(tif, TIFFTAG_YPOSITION,   &ty);
    TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres);
    TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres);
    TIFFGetField(tif, 33300, &canvas_w);
    TIFFGetField(tif, 33301, &canvas_h);

    // Read scanlines
    const tmsize_t scanline_size = TIFFScanlineSize(tif);
    std::vector<uint8_t> buf(static_cast<size_t>(scanline_size));

    cv::Mat mat(static_cast<int>(h), static_cast<int>(w), CV_32FC4);

    for (uint32_t y = 0; y < h; ++y) {
        TIFFReadScanline(tif, buf.data(), y);
        cv::Vec4f* row = mat.ptr<cv::Vec4f>(static_cast<int>(y));

        if (is_gray && bps == 8) {
            const uint8_t* src = buf.data();
            for (uint32_t x = 0; x < w; ++x) {
                const float g = src[x * spp] * scale;
                const float a = has_alpha ? src[x * spp + 1] * scale : 1.0f;
                row[x] = {g, g, g, a};  // BGRA with B=G=R=gray
            }
        } else if (is_gray && bps == 16) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(buf.data());
            for (uint32_t x = 0; x < w; ++x) {
                const float g = src[x * spp] * scale;
                const float a = has_alpha ? src[x * spp + 1] * scale : 1.0f;
                row[x] = {g, g, g, a};
            }
        } else if (is_rgb && bps == 8) {
            const uint8_t* src = buf.data();
            for (uint32_t x = 0; x < w; ++x) {
                const float r = src[x * spp + 0] * scale;
                const float g = src[x * spp + 1] * scale;
                const float b = src[x * spp + 2] * scale;
                const float a = has_alpha ? src[x * spp + 3] * scale : 1.0f;
                row[x] = {b, g, r, a};  // BGRA
            }
        } else if (is_rgb && bps == 16) {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(buf.data());
            for (uint32_t x = 0; x < w; ++x) {
                const float r = src[x * spp + 0] * scale;
                const float g = src[x * spp + 1] * scale;
                const float b = src[x * spp + 2] * scale;
                const float a = has_alpha ? src[x * spp + 3] * scale : 1.0f;
                row[x] = {b, g, r, a};
            }
        }

        // MINISWHITE: invert gray values (alpha stays)
        if (photo == PHOTOMETRIC_MINISWHITE) {
            for (uint32_t x = 0; x < w; ++x) {
                row[x][0] = 1.0f - row[x][0];
                row[x][1] = 1.0f - row[x][1];
                row[x][2] = 1.0f - row[x][2];
            }
        }
    }

    TIFFClose(tif);

    TiffImage result;
    result.mat       = std::move(mat);
    result.x         = (xoff >= 0) ? xoff : static_cast<int>(std::round(tx * xres));
    result.y         = (yoff >= 0) ? yoff : static_cast<int>(std::round(ty * yres));
    result.canvas_w  = static_cast<int>(canvas_w);
    result.canvas_h  = static_cast<int>(canvas_h);
    result.grayscale = is_gray;
    result.path      = path;
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

    // Clip image rect to canvas bounds (handles negative offsets and overhang)
    const int sx = std::max(0, -img.x);         // source x start (skip if negative offset)
    const int sy = std::max(0, -img.y);
    const int dx = std::max(0, img.x);           // dest x start on canvas
    const int dy = std::max(0, img.y);
    const int w  = std::min(img.mat.cols - sx, canvas.width  - dx);
    const int h  = std::min(img.mat.rows - sy, canvas.height - dy);

    if (w > 0 && h > 0) {
        img.mat(cv::Rect(sx, sy, w, h)).copyTo(result(cv::Rect(dx, dy, w, h)));
    }
    return result;
}

void writeTiff(const std::string& path, const cv::Mat& mat, int compression) {
    // Convert to 8-bit
    cv::Mat u8;
    if (mat.depth() == CV_32F)
        mat.convertTo(u8, CV_8U, 255.0);
    else
        u8 = mat;

    const int ch = u8.channels();

    // OpenCV stores color as BGR(A); TIFF expects RGB(A) — swap for multi-channel.
    if (ch == 4)
        cv::cvtColor(u8, u8, cv::COLOR_BGRA2RGBA);
    else if (ch == 3)
        cv::cvtColor(u8, u8, cv::COLOR_BGR2RGB);

    TIFF* tif = TIFFOpen(path.c_str(), "w");
    if (!tif)
        throw std::runtime_error("Cannot write: " + path);

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      static_cast<uint32_t>(u8.cols));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     static_cast<uint32_t>(u8.rows));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(ch));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   static_cast<uint16_t>(8));
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     ch >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tif, TIFFTAG_COMPRESSION,     COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_ZIPQUALITY,      compression);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP,    TIFFDefaultStripSize(tif, 0));

    if (ch == 4 || ch == 2) {
        uint16_t extra[] = { EXTRASAMPLE_UNASSALPHA };
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, extra);
    }

    for (int y = 0; y < u8.rows; ++y)
        TIFFWriteScanline(tif, u8.ptr(y), static_cast<uint32_t>(y));

    TIFFClose(tif);
}

} // namespace tiffio
