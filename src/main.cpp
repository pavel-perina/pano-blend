#include <print>
#include <string>
#include <vector>

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
    bool                  seam_verbose = false;
};

static void usage(const char* argv0) {
    std::println(stderr,
        "Usage: {} img1.tif [-xoff N] [-yoff N] img2.tif [-xoff N] [-yoff N] -o out.tif",
        argv0);
    std::println(stderr, "  -xoff/-yoff    pixel offset for the preceding image (overrides TIFF tags)");
    std::println(stderr, "  -o / --output  output TIFF");
    std::println(stderr, "  -SeamVerbose   write error.tif / seam.tif / seam_viz.tif");
    std::println(stderr, "  -w -v          accepted and ignored (enblend compat)");
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
            // -xoff after the image filename applies to the LAST parsed image.
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
            // Flush any pending xoff/yoff that appeared BEFORE the filename
            // (enblend style: -xoff N img.tif).  SmartBlend puts them AFTER,
            // but we support both for compatibility.
            if (pending_xoff >= 0) { inp.xoff = pending_xoff; pending_xoff = -1; }
            if (pending_yoff >= 0) { inp.yoff = pending_yoff; pending_yoff = -1; }
            opts.inputs.push_back(std::move(inp));
        }
    }
    return opts;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    Options opts = parseArgs(argc, argv);

    if (opts.inputs.size() != 2) {
        std::println(stderr, "error: exactly 2 input images required (got {})",
                     opts.inputs.size());
        usage(argv[0]);
        return 1;
    }
    if (opts.output.empty()) {
        std::println(stderr, "error: -o <output.tif> required");
        usage(argv[0]);
        return 1;
    }

    // --- Read inputs ---
    tiffio::TiffImage t1, t2;
    try {
        t1 = tiffio::readTiff(opts.inputs[0].path, opts.inputs[0].xoff, opts.inputs[0].yoff);
        t2 = tiffio::readTiff(opts.inputs[1].path, opts.inputs[1].xoff, opts.inputs[1].yoff);
    } catch (const std::exception& e) {
        std::println(stderr, "error: {}", e.what());
        return 1;
    }

    // --- Compute canvas and place images ---
    const cv::Size canvas = tiffio::canvasSize({ t1, t2 });
    std::println("Canvas: {}x{}", canvas.width, canvas.height);
    std::println("  {} at ({},{})", t1.path, t1.x, t1.y);
    std::println("  {} at ({},{})", t2.path, t2.x, t2.y);

    const cv::Mat f1 = tiffio::placeOnCanvas(t1, canvas);
    const cv::Mat f2 = tiffio::placeOnCanvas(t2, canvas);

    // --- Pipeline ---
    const cv::Mat err  = seam::computeError(f1, f2);
    const cv::Mat mask = seam::findSeam(f1, f2, err);

    if (opts.seam_verbose) {
        tiffio::writeTiff("error.tif",   err);
        tiffio::writeTiff("seam.tif",    mask);
        const cv::Mat viz = seam::visualizeSeam(f1, f2, err, mask);
        tiffio::writeTiff("seam_viz.tif", viz);
        std::println("Wrote error.tif, seam.tif, seam_viz.tif");
    }

    const cv::Mat blended = blend::multiBandBlend(f1, f2, mask);

    try {
        tiffio::writeTiff(opts.output, blended);
    } catch (const std::exception& e) {
        std::println(stderr, "error: {}", e.what());
        return 1;
    }
    std::println("Written: {}", opts.output);
    return 0;
}
