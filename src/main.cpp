#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <regex>
#include <string>
#include <vector>

static long long ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#include <opencv2/imgproc.hpp>

#include "blend.h"
#include "colors.h"
#include "labelmap.h"
#include "seam.h"
#include "tiff_io.h"

// version.h is generated at build time by cmake/GenerateVersion.cmake from
// `git describe`; the fallback keeps non-CMake builds compiling.
#if __has_include("version.h")
#include "version.h"
#endif
#ifndef PANOBLEND_VERSION
#define PANOBLEND_VERSION "unknown"
#endif

// ---------------------------------------------------------------------------
// CLI parser
// ---------------------------------------------------------------------------

struct InputArg {
    std::string path;
    int xoff = tiffio::kNoPos;  // kNoPos = read from TIFF tags
    int yoff = tiffio::kNoPos;
};

struct CanvasGeometry {
    int width  = 0;
    int height = 0;
    int xoff   = 0;
    int yoff   = 0;
};

struct Options {
    std::vector<InputArg> inputs;
    std::string           output;
    std::string           seam_mask_only;  // if set, write label map and exit
    std::string           load_label_map;  // if set, blend from this label map
    bool                  seam_verbose = false;
    CanvasGeometry        canvas_geom;     // -f geometry override (0 = auto)
};

static void usage(const char* argv0) {
    std::println(stderr,
        "Usage: {} img1.tif [-xoff N] [-yoff N] img2.tif ... -o out.tif",
        argv0);
    std::println(stderr, "  -xoff/-yoff      pixel offset for the preceding image (overrides TIFF tags)");
    std::println(stderr, "  -o / --output    output TIFF");
    std::println(stderr, "  -f WxH+X+Y       force canvas geometry (enblend compat, no space ok)");
    std::println(stderr, "  -SeamMaskOnly F  write label map to F and exit (no blending)");
    std::println(stderr, "  -LoadLabelMap F  blend using a label map from -SeamMaskOnly (skips seam finding)");
    std::println(stderr, "  -SeamVerbose     write per-step debug TIFFs (error/seam/seam_viz) + labelmap_viz/legend");
    std::println(stderr, "  @file            read arguments from a response file, one per line");
    std::println(stderr, "  -w [MODE]        wrap mode; accepted, wrap blending not implemented");
    std::println(stderr, "  -v               accepted and ignored (enblend compat)");
    std::println(stderr, "  --version        print version and exit");
}

// Options that consume one extra argument (value).
static const char* kValueOpts[] = {
    "-DER", "-DEC", "-MinSize", "-MaxComp", "-TSmooth",
    "-HiPassLevel", "-WrapMode", nullptr
};

static bool isValueOpt(const std::string& s) {
    for (int i = 0; kValueOpts[i]; ++i)
        if (s == kValueOpts[i]) return true;
    return false;
}

// enblend wrap modes (-w [MODE] / --wrap[=MODE]). A following argument is
// consumed as the mode only when it matches one of these, so `-w img2.tif`
// keeps img2.tif as an input.
static bool isWrapMode(const std::string& s) {
    return s == "none" || s == "open" || s == "horizontal" ||
           s == "vertical" || s == "both" || s == "all";
}

// enblend-compatible response files: an argument `@list.txt` names a file with
// one argument per line ('#' lines are comments; a line may nest another
// @file). Hugin uses these to dodge the Windows command-line length limit on
// projects with many images, so files may be CRLF-terminated.
static void expandArg(const std::string& arg, std::vector<std::string>& out, int depth) {
    if (arg.size() < 2 || arg[0] != '@') {
        out.push_back(arg);
        return;
    }
    if (depth >= 5) {
        std::println(stderr, "error: response files nested deeper than 5 levels at '{}'", arg);
        std::exit(1);
    }
    const std::string path = arg.substr(1);
    std::ifstream f(path);
    if (!f) {
        std::println(stderr, "error: cannot open response file '{}'", path);
        std::exit(1);
    }
    std::string line;
    while (std::getline(f, line)) {
        const size_t first = line.find_first_not_of(" \t\r");
        const size_t last  = line.find_last_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        expandArg(line.substr(first, last - first + 1), out, depth + 1);
    }
}

// Parse an integer option value; exits with a clear message instead of an
// uncaught std::stoi exception on garbage like `-xoff abc`.
static int parseInt(const char* opt, const std::string& val) {
    try {
        size_t pos = 0;
        const int v = std::stoi(val, &pos);
        if (pos != val.size()) throw std::invalid_argument(val);
        return v;
    } catch (const std::exception&) {
        std::println(stderr, "error: {}: invalid integer '{}'", opt, val);
        std::exit(1);
    }
}

// Parse X11/ImageMagick geometry: WIDTHxHEIGHT+XOFF+YOFF
// Offsets may be negative, written either "-12" or ImageMagick-style "+-12".
// Returns true on success.
static bool parseGeometry(const std::string& s, CanvasGeometry& g) {
    static const std::regex re(R"((\d+)x(\d+)([+-]-?\d+)([+-]-?\d+))");
    std::smatch m;
    if (!std::regex_match(s, m, re)) return false;
    auto geomInt = [](std::string v) {
        if (v[0] == '+') v.erase(0, 1);  // "+-12" → "-12", "+12" → "12"
        return std::stoi(v);
    };
    g.width  = std::stoi(m[1]);
    g.height = std::stoi(m[2]);
    g.xoff   = geomInt(m[3]);
    g.yoff   = geomInt(m[4]);
    return true;
}

static Options parseArgs(int argc, char** argv) {
    Options opts;
    int     pending_xoff = tiffio::kNoPos;
    int     pending_yoff = tiffio::kNoPos;

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        expandArg(argv[i], args, 0);

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];

        if (a == "-version" || a == "--version") {
            std::println("pano-blend {}", PANOBLEND_VERSION);
            std::println("https://github.com/pavel-perina/pano-blend");
            std::println("MIT License");
            std::exit(0);
        } else if (a == "-o" || a == "--output") {
            if (i + 1 >= args.size()) { std::println(stderr, "{}: requires argument", a); std::exit(1); }
            opts.output = args[++i];
        } else if (a == "-xoff") {
            if (i + 1 >= args.size()) { std::println(stderr, "-xoff: requires argument"); std::exit(1); }
            pending_xoff = parseInt("-xoff", args[++i]);
            if (!opts.inputs.empty()) {
                opts.inputs.back().xoff = pending_xoff;
                pending_xoff = tiffio::kNoPos;
            }
        } else if (a == "-yoff") {
            if (i + 1 >= args.size()) { std::println(stderr, "-yoff: requires argument"); std::exit(1); }
            pending_yoff = parseInt("-yoff", args[++i]);
            if (!opts.inputs.empty()) {
                opts.inputs.back().yoff = pending_yoff;
                pending_yoff = tiffio::kNoPos;
            }
        } else if (a == "-f" ||
                   (a.starts_with("-f") && a.size() > 2 &&
                    std::isdigit(static_cast<unsigned char>(a[2])))) {
            // -f WxH+X+Y  or  -fWxH+X+Y (no space)
            std::string geom_str;
            if (a == "-f") {
                if (i + 1 >= args.size()) { std::println(stderr, "-f: requires geometry argument"); std::exit(1); }
                geom_str = args[++i];
            } else {
                geom_str = a.substr(2);  // strip "-f"
            }
            if (!parseGeometry(geom_str, opts.canvas_geom)) {
                std::println(stderr, "error: invalid geometry '{}' (expected WxH+X+Y)", geom_str);
                std::exit(1);
            }
            if (opts.canvas_geom.width <= 0 || opts.canvas_geom.height <= 0) {
                std::println(stderr, "error: canvas dimensions must be positive (got {}x{})",
                             opts.canvas_geom.width, opts.canvas_geom.height);
                std::exit(1);
            }
        } else if (a == "-SeamMaskOnly") {
            if (i + 1 >= args.size()) { std::println(stderr, "-SeamMaskOnly: requires argument"); std::exit(1); }
            opts.seam_mask_only = args[++i];
        } else if (a == "-LoadLabelMap") {
            if (i + 1 >= args.size()) { std::println(stderr, "-LoadLabelMap: requires argument"); std::exit(1); }
            opts.load_label_map = args[++i];
        } else if (a == "-SeamVerbose") {
            opts.seam_verbose = true;
        } else if (a == "-w" || a == "--wrap" || a.starts_with("--wrap=")) {
            // enblend wrap; bare -w means horizontal (enblend's default). The
            // mode word after -w is consumed only if it is a known mode, so an
            // input filename after a bare -w is not eaten.
            std::string mode = "horizontal";
            if (a.starts_with("--wrap="))
                mode = a.substr(7);
            else if (i + 1 < args.size() && isWrapMode(args[i + 1]))
                mode = args[++i];
            if (mode != "none" && mode != "open")
                std::println(stderr,
                             "warning: -w {}: wrap blending not implemented; "
                             "seams will not cross the canvas edge", mode);
        } else if (a == "-v" || a == "-PyramidVerbose" || a == "-HorWrap") {
            // accepted, ignored
        } else if (isValueOpt(a)) {
            if (i + 1 >= args.size()) { std::println(stderr, "{}: requires argument", a); std::exit(1); }
            std::println(stderr, "warning: {} not implemented, ignoring", a);
            ++i;  // skip value
        } else if (!a.empty() && a[0] == '-') {
            std::println(stderr, "warning: unknown option '{}', ignoring", a);
        } else {
            // Positional: input file
            InputArg inp;
            inp.path = a;
            if (pending_xoff != tiffio::kNoPos) { inp.xoff = pending_xoff; pending_xoff = tiffio::kNoPos; }
            if (pending_yoff != tiffio::kNoPos) { inp.yoff = pending_yoff; pending_yoff = tiffio::kNoPos; }
            opts.inputs.push_back(std::move(inp));
        }
    }
    return opts;
}

// Distinct BGRA colour per label index: label 0 → transparent; labels 1..N get
// golden-angle hues in OkLCh at fixed L/C. Not pixel-identical to
// tools/colorize_mask.py (which uses the toe-corrected OkLrCh via coloraide) —
// a separate, equally-distinct palette built on the plain OkLCh in colors.h.
static std::vector<cv::Vec4b> labelPalette(int N) {
    constexpr float kGoldenDeg = 137.50776f;  // 180 * (3 - sqrt(5))
    std::vector<cv::Vec4b> palette(N + 1, cv::Vec4b(0, 0, 0, 0));  // label 0 = transparent
    float hue = 0.0f;
    for (int i = 1; i <= N; ++i) {
        const color::Rgb8 c = color::rgb8FromOkLch({0.66f, 0.12f, hue});
        palette[i] = cv::Vec4b(c.b, c.g, c.r, 255);  // BGRA
        hue = std::fmod(hue + kGoldenDeg, 360.0f);
    }
    return palette;
}

// Map a label map (0=uncovered, 1..N=image index) to a BGRA image via the
// palette. labelmap::accumulate only writes labels 0..N and the palette is
// sized N+1, so the lookup needs no range check.
static cv::Mat colorizeLabelMap(const cv::Mat& label, const std::vector<cv::Vec4b>& palette) {
    cv::Mat out(label.size(), CV_8UC4);
    for (int y = 0; y < label.rows; ++y) {
        const uint16_t* lbl = label.ptr<uint16_t>(y);
        cv::Vec4b*      dst = out.ptr<cv::Vec4b>(y);
        for (int x = 0; x < label.cols; ++x)
            dst[x] = palette[lbl[x]];
    }
    return out;
}

// Render a legend keying the golden-angle palette to input names: each name is
// drawn next to its colour swatch on an opaque black background.
static cv::Mat renderLabelLegend(const std::vector<std::string>& names,
                                 const std::vector<cv::Vec4b>& palette) {
    constexpr int    kFont   = cv::FONT_HERSHEY_SIMPLEX;
    constexpr double kScale  = 0.5;
    constexpr int    kThick  = 1;
    constexpr int    kPad    = 8, kSwatch = 16, kGap = 8, kRow = 24;

    int maxTextW = 0, textH = 8;
    for (const auto& n : names) {
        int base = 0;
        const cv::Size sz = cv::getTextSize(n, kFont, kScale, kThick, &base);
        maxTextW = std::max(maxTextW, sz.width);
        textH    = std::max(textH, sz.height);
    }
    const int W = kPad + kSwatch + kGap + maxTextW + kPad;
    const int H = kPad + static_cast<int>(names.size()) * kRow + kPad;
    cv::Mat out(H, W, CV_8UC4, cv::Scalar(0, 0, 0, 255));  // opaque black

    for (size_t i = 0; i < names.size(); ++i) {
        const cv::Vec4b col = palette[i + 1];
        const cv::Scalar sc(col[0], col[1], col[2], 255);
        const int y = kPad + static_cast<int>(i) * kRow;
        cv::rectangle(out, cv::Rect(kPad, y, kSwatch, kSwatch), sc, cv::FILLED);
        cv::putText(out, names[i], cv::Point(kPad + kSwatch + kGap, y + (kSwatch + textH) / 2),
                    kFont, kScale, sc, kThick, cv::LINE_AA);
    }
    return out;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    Options opts = parseArgs(argc, argv);

    if (opts.inputs.size() < 2) {
        std::println(stderr, "error: at least 2 input images required (got {})",
                     opts.inputs.size());
        usage(argv[0]);
        return 1;
    }
    if (opts.inputs.size() > 65535) {  // 16-bit label map
        std::println(stderr, "error: at most 65535 input images supported (got {})",
                     opts.inputs.size());
        return 1;
    }
    if (opts.output.empty() && opts.seam_mask_only.empty()) {
        std::println(stderr, "error: -o <output.tif> or -SeamMaskOnly <mask.tif> required");
        usage(argv[0]);
        return 1;
    }

    const int N = static_cast<int>(opts.inputs.size());

    // --- Read inputs ---
    auto t0 = ms();
    std::vector<tiffio::TiffImage> images(N);
    try {
        for (int i = 0; i < N; ++i)
            images[i] = tiffio::readTiff(opts.inputs[i].path, opts.inputs[i].xoff, opts.inputs[i].yoff);
    } catch (const std::exception& e) {
        std::println(stderr, "error: {}", e.what());
        return 1;
    }
    std::println("  read:         {:6} ms  ({} images)", ms() - t0, N);

    // --- Compute canvas and place images ---
    t0 = ms();
    cv::Size canvas = tiffio::canvasSize(images);

    // Apply -f geometry override
    if (opts.canvas_geom.width > 0) {
        const auto& g = opts.canvas_geom;
        // Shift all image positions by the geometry offset
        for (int i = 0; i < N; ++i) {
            images[i].x -= g.xoff;
            images[i].y -= g.yoff;
        }
        canvas = {g.width, g.height};

        // Validate: warn if any image falls entirely outside the canvas
        for (int i = 0; i < N; ++i) {
            const int ix1 = images[i].x + images[i].mat.cols;
            const int iy1 = images[i].y + images[i].mat.rows;
            if (ix1 <= 0 || iy1 <= 0 || images[i].x >= g.width || images[i].y >= g.height) {
                std::println(stderr, "warning: '{}' at ({},{}) falls outside canvas {}x{}",
                             images[i].path, images[i].x, images[i].y, g.width, g.height);
            }
        }
        std::println("Canvas: {}x{} (from -f, offset {},{})", canvas.width, canvas.height, g.xoff, g.yoff);
    } else {
        std::println("Canvas: {}x{}", canvas.width, canvas.height);
    }
    for (int i = 0; i < N; ++i)
        std::println("  {} at ({},{})", images[i].path, images[i].x, images[i].y);

    std::vector<cv::Mat>  canvas_images(N);
    std::vector<cv::Rect> rects(N);
    bool grayscale = true;
    for (int i = 0; i < N; ++i) {
        rects[i] = cv::Rect(images[i].x, images[i].y,
                            images[i].mat.cols, images[i].mat.rows);
        grayscale = grayscale && images[i].grayscale;
        canvas_images[i] = tiffio::placeOnCanvas(images[i], canvas);
        images[i].mat.release();  // original crop no longer needed
    }
    std::println("  place:        {:6} ms", ms() - t0);

    cv::Mat label_map;
    if (!opts.load_label_map.empty()) {
        // --- Frozen label map from a previous -SeamMaskOnly run ---
        t0 = ms();
        try {
            const tiffio::TiffImage lm = tiffio::readTiff(opts.load_label_map);
            if (lm.mat.size() != canvas)
                throw std::runtime_error(std::format(
                    "label map '{}' is {}x{} but the canvas is {}x{}",
                    opts.load_label_map, lm.mat.cols, lm.mat.rows,
                    canvas.width, canvas.height));
            // readTiff normalizes to CV_32FC4 (gray → R=G=B, /65535); float32
            // holds 16-bit integers exactly, so rounding recovers the labels.
            label_map.create(canvas, CV_16UC1);
            for (int y = 0; y < canvas.height; ++y) {
                const cv::Vec4f* src = lm.mat.ptr<cv::Vec4f>(y);
                uint16_t*        lbl = label_map.ptr<uint16_t>(y);
                for (int x = 0; x < canvas.width; ++x) {
                    const long v = std::lround(src[x][0] * 65535.0f);
                    if (v > N)
                        throw std::runtime_error(std::format(
                            "label map '{}' has label {} but only {} inputs "
                            "(expects the 16-bit map written by -SeamMaskOnly)",
                            opts.load_label_map, v, N));
                    lbl[x] = static_cast<uint16_t>(v);
                }
            }
        } catch (const std::exception& e) {
            std::println(stderr, "error: {}", e.what());
            return 1;
        }
        std::println("  label map:    {:6} ms  (loaded {})", ms() - t0, opts.load_label_map);
    } else {
        // --- Placement order: Prim max-overlap spanning tree from the center ---
        const std::vector<int> order = labelmap::placementOrder(rects);
        {
            std::string s;
            for (int idx : order) s += std::format(" {}", idx);
            std::println("  Placement order:{}", s);
        }

        // --- Sequential-accumulate label map: N-1 cuts, one frozen order ---
        t0 = ms();
        labelmap::StepCallback verbose_cb;
        if (opts.seam_verbose) {
            verbose_cb = [&](int step, int idx, const cv::Mat& err,
                             const cv::Mat& mask, const cv::Mat& mosaic) {
                const auto tv = ms();
                std::string ef = "error.tif", sf = "seam.tif", vf = "seam_viz.tif";
                if (N > 2) {
                    ef = std::format("error_{}_{}.tif", step, idx);
                    sf = std::format("seam_{}_{}.tif", step, idx);
                    vf = std::format("seam_viz_{}_{}.tif", step, idx);
                }
                tiffio::writeTiff(ef, err);
                tiffio::writeTiff(sf, mask);
                tiffio::writeTiff(vf, seam::visualizeSeam(mosaic, canvas_images[idx], err, mask));
                std::println("    seam verbose: {:6} ms  ({}, {}, {})", ms() - tv, ef, sf, vf);
            };
        }
        label_map = labelmap::accumulate(canvas_images, rects, order, grayscale, verbose_cb);
        std::println("  label map:    {:6} ms", ms() - t0);
    }

    if (opts.seam_verbose) {
        t0 = ms();
        const std::vector<cv::Vec4b> palette = labelPalette(N);
        tiffio::writeTiff("labelmap_viz.tif", colorizeLabelMap(label_map, palette));

        std::vector<std::string> names;
        names.reserve(images.size());
        for (const auto& im : images)
            names.push_back(std::filesystem::path(im.path).filename().string());
        tiffio::writeTiff("labelmap_legend.tif", renderLabelLegend(names, palette));
        std::println("  label verbose:{:6} ms  (labelmap_viz.tif, labelmap_legend.tif)", ms() - t0);
    }

    // --- SeamMaskOnly: write and exit ---
    if (!opts.seam_mask_only.empty()) {
        t0 = ms();
        try {
            tiffio::writeTiff(opts.seam_mask_only, label_map);
        } catch (const std::exception& e) {
            std::println(stderr, "error: {}", e.what());
            return 1;
        }
        std::println("  write mask:   {:6} ms", ms() - t0);
        std::println("Written: {}", opts.seam_mask_only);
        return 0;
    }

    // --- Multi-band blend ---
    t0 = ms();
    const cv::Mat blended = blend::multiBandBlend(canvas_images, label_map);
    std::println("  blend:        {:6} ms", ms() - t0);

    t0 = ms();
    try {
        tiffio::writeTiff(opts.output, blended);
    } catch (const std::exception& e) {
        std::println(stderr, "error: {}", e.what());
        return 1;
    }
    std::println("  write:        {:6} ms", ms() - t0);
    std::println("Written: {}", opts.output);
    return 0;
}
