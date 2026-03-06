#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace tiffio {

struct TiffImage {
    cv::Mat     mat;        // CV_32FC4 float BGRA (image crop, NOT full canvas)
    int         x = 0;     // x offset in canvas (from tags or -xoff)
    int         y = 0;     // y offset in canvas (from tags or -yoff)
    int         canvas_w = 0;  // Pixar IMAGEFULLWIDTH tag (0 = not set)
    int         canvas_h = 0;  // Pixar IMAGEFULLLENGTH tag (0 = not set)
    std::string path;
};

// Read a TIFF file and return it with placement metadata.
// Currently supports 8-bit RGB (→ opaque RGBA) and 8-bit RGBA only.
// Other formats (16-bit, grayscale, GA) print "Not implemented" to stderr
// and throw std::runtime_error.
// xoff / yoff: override position; -1 means read from TIFF tags (default 0).
TiffImage readTiff(const std::string& path, int xoff = -1, int yoff = -1);

// Compute full canvas dimensions from a set of images.
// Uses Pixar IMAGEFULLWIDTH/FULLLENGTH if present, otherwise derives from
// image positions and native sizes.
cv::Size canvasSize(const std::vector<TiffImage>& images);

// Create a full-canvas transparent image and paste img.mat at (img.x, img.y).
// Returns CV_32FC4.
cv::Mat placeOnCanvas(const TiffImage& img, cv::Size canvas);

// Write a mat to a TIFF file (Deflate compressed, alpha-tagged).
// Accepts CV_8UC1/3/4 or CV_32FC1/3/4 (auto-converted to 8-bit).
// compression: 1=fastest … 9=smallest (zlib level, default 6).
void writeTiff(const std::string& path, const cv::Mat& mat, int compression = 6);

} // namespace tiffio
