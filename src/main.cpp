#include <chrono>
#include <print>
#include <string>
#include <vector>

static long long ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#include "blend.h"
#include "seam.h"
#include "tiff_io.h"

// ---------------------------------------------------------------------------
// CLI parser
// ---------------------------------------------------------------------------

struct InputArg {
    std::string path;
    int xoff = -1;  // -1 = read from TIFF tags
    int yoff = -1;
};

struct Options {
    std::vector<InputArg> inputs;
    std::string           output;
    std::string           seam_mask_only;  // if set, write label map and exit
    bool                  seam_verbose = false;
};

static void usage(const char* argv0) {
    std::println(stderr,
        "Usage: {} img1.tif [-xoff N] [-yoff N] img2.tif ... -o out.tif",
        argv0);
    std::println(stderr, "  -xoff/-yoff      pixel offset for the preceding image (overrides TIFF tags)");
    std::println(stderr, "  -o / --output    output TIFF");
    std::println(stderr, "  -SeamMaskOnly F  write label map to F and exit (no blending)");
    std::println(stderr, "  -SeamVerbose     write per-pair debug TIFFs (error/seam/seam_viz)");
    std::println(stderr, "  -w -v            accepted and ignored (enblend compat)");
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

static Options parseArgs(int argc, char** argv) {
    Options opts;
    int     pending_xoff = -1;
    int     pending_yoff = -1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "-o" || a == "--output") {
            if (i + 1 >= argc) { std::println(stderr, "{}: requires argument", a); std::exit(1); }
            opts.output = argv[++i];
        } else if (a == "-xoff") {
            if (i + 1 >= argc) { std::println(stderr, "-xoff: requires argument"); std::exit(1); }
            pending_xoff = std::stoi(argv[++i]);
            if (!opts.inputs.empty()) {
                opts.inputs.back().xoff = pending_xoff;
                pending_xoff = -1;
            }
        } else if (a == "-yoff") {
            if (i + 1 >= argc) { std::println(stderr, "-yoff: requires argument"); std::exit(1); }
            pending_yoff = std::stoi(argv[++i]);
            if (!opts.inputs.empty()) {
                opts.inputs.back().yoff = pending_yoff;
                pending_yoff = -1;
            }
        } else if (a == "-SeamMaskOnly") {
            if (i + 1 >= argc) { std::println(stderr, "-SeamMaskOnly: requires argument"); std::exit(1); }
            opts.seam_mask_only = argv[++i];
        } else if (a == "-SeamVerbose") {
            opts.seam_verbose = true;
        } else if (a == "-w" || a == "-v" ||
                   a == "-PyramidVerbose" || a == "-HorWrap") {
            // accepted, ignored
        } else if (isValueOpt(a)) {
            if (i + 1 >= argc) { std::println(stderr, "{}: requires argument", a); std::exit(1); }
            std::println(stderr, "warning: {} not implemented, ignoring", a);
            ++i;  // skip value
        } else if (!a.empty() && a[0] == '-') {
            std::println(stderr, "warning: unknown option '{}', ignoring", a);
        } else {
            // Positional: input file
            InputArg inp;
            inp.path = a;
            if (pending_xoff >= 0) { inp.xoff = pending_xoff; pending_xoff = -1; }
            if (pending_yoff >= 0) { inp.yoff = pending_yoff; pending_yoff = -1; }
            opts.inputs.push_back(std::move(inp));
        }
    }
    return opts;
}

// ---------------------------------------------------------------------------
// Overlap detection
// ---------------------------------------------------------------------------

struct OverlapPair {
    int i, j;           // image indices
    cv::Rect overlap;   // bounding box in canvas coordinates
};

// Find all pairs of images whose bounding boxes overlap.
static std::vector<OverlapPair> findOverlaps(const std::vector<tiffio::TiffImage>& images) {
    std::vector<OverlapPair> pairs;
    const int N = static_cast<int>(images.size());
    for (int i = 0; i < N; ++i) {
        cv::Rect ri(images[i].x, images[i].y, images[i].mat.cols, images[i].mat.rows);
        for (int j = i + 1; j < N; ++j) {
            cv::Rect rj(images[j].x, images[j].y, images[j].mat.cols, images[j].mat.rows);
            cv::Rect inter = ri & rj;
            if (inter.area() > 0) {
                pairs.push_back({i, j, inter});
            }
        }
    }
    return pairs;
}

// ---------------------------------------------------------------------------
// Label map construction from pairwise seams
// ---------------------------------------------------------------------------

// Build a label map (0=no image, 1..N=image index) from pairwise seam masks.
// Uses sequential priority: later pairs override earlier assignments.
static cv::Mat buildLabelMap(const std::vector<cv::Mat>& canvas_images,
                             const std::vector<OverlapPair>& pairs,
                             const std::vector<cv::Mat>& seam_masks,
                             cv::Size canvas) {
    const int N = static_cast<int>(canvas_images.size());

    // Initialize: assign each pixel to the first image that covers it
    cv::Mat label(canvas, CV_8UC1, cv::Scalar(0));
    for (int i = 0; i < N; ++i) {
        for (int y = 0; y < canvas.height; ++y) {
            const cv::Vec4f* row = canvas_images[i].ptr<cv::Vec4f>(y);
            uint8_t*         lbl = label.ptr<uint8_t>(y);
            for (int x = 0; x < canvas.width; ++x) {
                if (row[x][3] > 0.5f && lbl[x] == 0)
                    lbl[x] = static_cast<uint8_t>(i + 1);
            }
        }
    }

    // Apply pairwise seams: each seam overrides the label in overlap regions
    for (size_t p = 0; p < pairs.size(); ++p) {
        const auto& pair = pairs[p];
        const cv::Mat& mask = seam_masks[p];  // 0=image i, 255=image j
        const int img_i = pair.i + 1;  // 1-based label
        const int img_j = pair.j + 1;

        for (int y = 0; y < canvas.height; ++y) {
            const uint8_t* rm  = mask.ptr<uint8_t>(y);
            uint8_t*       lbl = label.ptr<uint8_t>(y);
            for (int x = 0; x < canvas.width; ++x) {
                // Only modify pixels that belong to one of the two images in this pair
                if (lbl[x] != img_i && lbl[x] != img_j) continue;
                lbl[x] = (rm[x] == 0) ? img_i : img_j;
            }
        }
    }

    return label;
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
    const cv::Size canvas = tiffio::canvasSize(images);
    std::println("Canvas: {}x{}", canvas.width, canvas.height);
    for (int i = 0; i < N; ++i)
        std::println("  {} at ({},{})", images[i].path, images[i].x, images[i].y);

    std::vector<cv::Mat> canvas_images(N);
    for (int i = 0; i < N; ++i)
        canvas_images[i] = tiffio::placeOnCanvas(images[i], canvas);
    std::println("  place:        {:6} ms", ms() - t0);

    // --- Find overlapping pairs ---
    const auto pairs = findOverlaps(images);
    std::println("  Overlapping pairs: {}", pairs.size());
    for (const auto& p : pairs)
        std::println("    images {}-{}: overlap {}x{} at ({},{})",
                     p.i, p.j, p.overlap.width, p.overlap.height,
                     p.overlap.x, p.overlap.y);

    if (pairs.empty()) {
        std::println(stderr, "warning: no overlapping image pairs found");
    }

    // --- Pairwise seam finding ---
    std::vector<cv::Mat> seam_masks(pairs.size());
    for (size_t p = 0; p < pairs.size(); ++p) {
        const auto& pair = pairs[p];
        std::println("  Seam {}-{}:", pair.i, pair.j);

        t0 = ms();
        const cv::Mat err = seam::computeError(canvas_images[pair.i], canvas_images[pair.j]);
        std::println("    computeError: {:6} ms", ms() - t0);

        t0 = ms();
        seam_masks[p] = seam::findSeam(canvas_images[pair.i], canvas_images[pair.j], err);
        std::println("    findSeam:     {:6} ms", ms() - t0);

        if (opts.seam_verbose) {
            t0 = ms();
            if (pairs.size() == 1) {
                tiffio::writeTiff("error.tif",    err,  9);
                tiffio::writeTiff("seam.tif",     seam_masks[p], 9);
                const cv::Mat viz = seam::visualizeSeam(canvas_images[pair.i], canvas_images[pair.j],
                                                        err, seam_masks[p]);
                tiffio::writeTiff("seam_viz.tif", viz,  9);
                std::println("    seam verbose: {:6} ms  (error.tif, seam.tif, seam_viz.tif)", ms() - t0);
            } else {
                auto ef = std::format("error_{}_{}.tif", pair.i, pair.j);
                auto sf = std::format("seam_{}_{}.tif", pair.i, pair.j);
                auto vf = std::format("seam_viz_{}_{}.tif", pair.i, pair.j);
                tiffio::writeTiff(ef, err,  9);
                tiffio::writeTiff(sf, seam_masks[p], 9);
                const cv::Mat viz = seam::visualizeSeam(canvas_images[pair.i], canvas_images[pair.j],
                                                        err, seam_masks[p]);
                tiffio::writeTiff(vf, viz,  9);
                std::println("    seam verbose: {:6} ms  ({}, {}, {})", ms() - t0, ef, sf, vf);
            }
        }
    }

    // --- Build label map ---
    t0 = ms();
    const cv::Mat label_map = buildLabelMap(canvas_images, pairs, seam_masks, canvas);
    std::println("  label map:    {:6} ms", ms() - t0);

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
