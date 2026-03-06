# pano-blend — Project Context

## What this is
C++23 / OpenCV panoramic image blender. Finds optimal seams via Boykov-Kolmogorov
graph-cut, then composites using Laplacian pyramid (multi-band) blending.
Supports N images. Algorithm reconstructed from SmartBlend (Michael Norel, 2007).
RE docs: `/mnt/c/dev-c/blend/smartblend/` (Windows FS — read-only reference).

## Environment
- OS: openSUSE Leap 16.0 in WSL2
- Compiler: clang++ 19.1.7, CMake 3.31.7, OpenCV 4.10.0
- Git repo + build on Linux FS (cmake/git cannot write to /mnt/c)

## Build
```
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
      -DOpenCV_DIR=/usr/lib64/cmake/opencv4
cmake --build build --parallel
ctest --test-dir build # runs GTest suite
```

## CLI
```
blend img1.tif img2.tif [img3.tif ...] -o out.tif
blend img1.tif img2.tif -SeamMaskOnly mask.tif   # label map only, no blending
blend img1.tif img2.tif -o out.tif -SeamVerbose   # debug TIFFs
```
- Image positions read from TIFF tags (XPOSITION/YPOSITION), or `-xoff`/`-yoff`
- `-SeamMaskOnly F` — write label map (0=uncovered, 1..N=image index) and exit
- `-SeamVerbose` — per-pair error/seam/seam_viz TIFFs (numbered for 3+ images)
- `-w`, `-v` accepted and ignored (enblend compat)
- SmartBlend flags (`-DER`, `-DEC`, `-MinSize`, etc.) accepted with warning

## Pipeline
| Step | Function | Description |
|---|---|---|
| 1 | `seam::computeError` | OKLab ΔE per pixel (pairwise) |
| 2 | `seam::findSeam` | Coarse-to-fine BK min-cut seam (8× downsample + 64px band refinement) |
| 3 | `buildLabelMap` | Combine pairwise seams into single label map (0..N) |
| 4 | `blend::multiBandBlend` | N-image Laplacian pyramid composite via label map |

## Key source files
```
src/main.cpp                — CLI parser, overlap detection, label map, pipeline driver
src/seam.h   / seam.cpp     — computeError, findSeam (coarse-to-fine), visualizeSeam
src/blend.h  / blend.cpp    — multiBandBlend (N images + label map → CV_8UC4)
src/tiff_io.h / tiff_io.cpp — readTiff (with position tags), writeTiff (libtiff, Deflate)
src/colors.h / colors.cpp   — OKLab/OKLCh conversions (H in degrees)
tests/test_input.cpp        — GTest: validates p1.tif / p2.tif pixel values
tools/tag_tiff.c            — standalone TIFF tag reader/writer
tools/colorize_mask.py      — OkLrCh palette visualization of label maps
```

## TIFF I/O
- `writeTiff()` uses libtiff directly (not OpenCV) for:
  - COMPRESSION_ADOBE_DEFLATE with configurable zlib level (1-9, default 6)
  - EXTRASAMPLE_UNASSALPHA tag for 4-channel images
  - BGRA→RGBA channel reorder (OpenCV stores BGR internally)
- `readTiff()` reads TIFFTAG_XPOSITION/YPOSITION for canvas placement

## Test data
- `test-data/p1.tif`, `p2.tif` — 405×240 RGBA float; p2 starts at x=85
- `test-data/mask.tif`, `mask_viz.tif` — label map from 2-image seam
- Large test: 3× DSCF photos (5000×3000 each) → 9784×4396 canvas, ~26s

## OpenCV gotchas
- `imread` always delivers BGR/BGRA regardless of file format
- OKLch H field is in **degrees** (see `convLchToLab` in colors.cpp)
- `cv::detail::GCGraph` is in `<opencv2/imgproc/detail/gcgraph.hpp>`
- `cv::detail::MultiBandBlender` needs `find_package(OpenCV … stitching)`

## What's next

### SmartBlend parity (not yet implemented)
1. **Distance error terms (DER/DEC)** — distance-based cost biasing seam toward
   overlap centre. `-DER 0.25` (relative), `-DEC 0.094` (constant).
2. **High-pass delta** — `-HiPassLevel 4` means sigma = overlapWidth/16 Gaussian.
   Do not implement without RE confirmation of exact formula.

### SEM pipeline (future)
- **Grayscale + 16-bit support**: SEM images are grayscale 16-bit.
  Need to handle CV_16UC1/CV_16UC2 on load, convert to float internally.
  For SEM (linear, no colour) use intensity diff instead of OKLab.
- **Large grid stitching**: 300×300 images → 90 Gpixel canvas.
  Architecture: global seam finding (lightweight), tiled/strip-based blending.
  `-SeamMaskOnly` enables external orchestration — call pairwise, collect masks,
  reconstruct output in tiles with a separate tool.

## SmartBlend RE findings
- **Seam finding**: BK graph-cut, pairwise, with coarse-to-fine multi-scale search.
- **Multiple images**: strictly pairwise sequential, no N-way blending.
- **Pyramid blend**: debug images (DBGP_Pyramid_1/2, DBGP_PyramidBlended) suggest a
  Burt-Adelson pyramid blend exists, but the RE traced call chain concluded hard cut.
  The enclosing function's prologue was never found — a blend pass may have been missed.
  Status: **unresolved**. Our MultiBandBlender covers this regardless.
- Full RE details: `/mnt/c/dev-c/blend/smartblend/08_FinalArchitecture.md`
