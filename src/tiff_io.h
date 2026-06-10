#pragma once

#include <opencv2/core.hpp>
#include <limits>
#include <string>
#include <vector>

namespace tiffio {

// Sentinel for "position not specified" — offsets themselves may be any
// value, including negative (e.g. after a -f canvas shift).
inline constexpr int kNoPos = std::numeric_limits<int>::min();

struct TiffImage {
    cv::Mat     mat;            // CV_32FC4 float BGRA (image crop, NOT full canvas)
    int         x = 0;         // x offset in canvas (from tags or -xoff)
    int         y = 0;         // y offset in canvas (from tags or -yoff)
    int         canvas_w = 0;  // Pixar IMAGEFULLWIDTH tag (0 = not set)
    int         canvas_h = 0;  // Pixar IMAGEFULLLENGTH tag (0 = not set)
    bool        grayscale = false;  // true if source was grayscale (R=G=B internally)
    std::string path;
};

// Read a TIFF file and return it with placement metadata.
// Supports 8-bit and 16-bit, grayscale/gray+alpha/RGB/RGBA.
// All formats are converted to CV_32FC4 BGRA float [0,1].
// Grayscale images get R=G=B=gray and grayscale flag set.
// xoff / yoff: override position (may be negative); kNoPos means read from
// TIFF tags (default 0).
TiffImage readTiff(const std::string& path, int xoff = kNoPos, int yoff = kNoPos);

// Compute full canvas dimensions from a set of images.
cv::Size canvasSize(const std::vector<TiffImage>& images);

// Create a full-canvas transparent image and paste img.mat at (img.x, img.y).
cv::Mat placeOnCanvas(const TiffImage& img, cv::Size canvas);

// Write a mat to a TIFF file (Deflate compressed with horizontal predictor,
// alpha-tagged).  Accepts CV_8UC1/3/4 or CV_32FC1/3/4 (auto-converted to 8-bit).
// compression: 1=fastest … 9=smallest (zlib level; default 1 — the predictor
// recovers most of the ratio and higher levels cost seconds on big canvases).
void writeTiff(const std::string& path, const cv::Mat& mat, int compression = 1);

} // namespace tiffio
