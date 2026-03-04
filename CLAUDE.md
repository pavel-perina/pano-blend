# pano-blend — Project Context

## What this is
C++23 / OpenCV panoramic image stitcher, progressively reconstructing the algorithm
of SmartBlend (Michael Norel, 2007). Reference binary + RE docs live at
`/mnt/c/dev-c/blend/smartblend/` (Windows FS — do not move).

## Environment
- OS: openSUSE Leap 16.0 in WSL2
- Compiler: clang++ 19.1.7, CMake 3.31.7, OpenCV 4.10.0
- Git repo + build on Linux FS (cmake/git cannot write to /mnt/c)

## Build
```
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
      -DOpenCV_DIR=/usr/lib64/cmake/opencv4
cmake --build build --parallel
./build/blend          # writes test-data/*.tif
ctest --test-dir build # runs GTest suite
```

## Pipeline (all implemented)
| Step | Function | Output |
|---|---|---|
| 1 | `seam::computeError` | OKLab ΔE per pixel | `error.tif` |
| 2 | `seam::findSeam` | BK min-cut seam mask | `seam.tif` |
| 3 | `seam::visualizeSeam` | OKLCh false-colour diagnostic | `seam_viz.tif` |
| 4 | `blend::multiBandBlend` | Laplacian pyramid composite | `blend.tif` |

## Key source files
```
src/colors.h / colors.cpp   — OKLab/OKLCh conversions (H in degrees)
src/seam.h   / seam.cpp     — computeError, findSeam, visualizeSeam
src/blend.h  / blend.cpp    — multiBandBlend (cv::detail::MultiBandBlender)
src/main.cpp                — pipeline driver
tests/test_input.cpp        — GTest: validates p1.tif / p2.tif pixel values
```

## Test data
- `test-data/p1.tif`, `p2.tif` — 405×240 RGBA float; p2 starts at x=85
- Source JPEGs: `/mnt/c/dev-c/blend/smartblend/p1.jpg` (319×240), `p2.jpg` (320×240)

## OpenCV gotchas
- `imread` always delivers BGR/BGRA regardless of file format
- TIFF writer swaps B↔R internally — pass BGRA directly, do NOT pre-convert
- OKLch H field is in **degrees** (see `convLchToLab` in colors.cpp)
- `cv::detail::GCGraph` is in `<opencv2/imgproc/detail/gcgraph.hpp>`
- `cv::detail::MultiBandBlender` needs `find_package(OpenCV … stitching)`

## What's next

### SmartBlend parity
1. **Distance error terms (DER/DEC)** — SmartBlend adds a distance-based cost to the
   seam n-weights: `-DER 0.25` (relative) and `-DEC 0.094` (constant). Likely
   implemented in `ComputeDirectPixelError` (0x4076b0). Probably distance from image
   boundary or overlap centre, biasing the seam toward the middle of the overlap.
2. **High-pass delta** — `-HiPassLevel 4` means sigma = overlapWidth/16 Gaussian blur
   on the colour difference before squaring. **Do not implement without RE confirmation
   of exact formula** — naive high-pass on raw ΔE made results worse (see git history).
3. **RE: `seekTo` (0x407c10)** — likely contains the delta channel computation.
4. **RE: `compositeBlend` (vtable[13])** — final pixel write-back / alpha handling.

### CLI / usability
- **Enblend-compatible CLI**: `blend img1 [-xoff N] img2 [-xoff N] img3 ... -o out.tif`
  - Positional args = input files; `-xoff`/`-yoff` follow each image
  - `-o` output file
  - `-w`, `-v` accepted and ignored (enblend compat)
  - `-SeamVerbose` → write error.tif / seam_viz.tif (already done internally)
  - `-MinSize` → num pyramid bands (currently hardcoded 5)
- **TIFF position tags** as alternative to `-xoff`/`-yoff`:
  - `TIFFTAG_XPOSITION` (286) / `TIFFTAG_YPOSITION` (287) — pixel offset in canvas
  - `TIFFTAG_PIXAR_IMAGEFULLWIDTH` (33300) / `TIFFTAG_PIXAR_IMAGEFULLLENGTH` (33301)
  - Written by Hugin/PTStitch; xposition_pixels = XPOSITION / XRESOLUTION
- **Exposure/colour correction** — `cv::detail::GainCompensator` (already linked via
  stitching module). Run before `computeError` on the overlap region.

## Future / TODO

### Grayscale + 16-bit support (SEM images)
Goal: reuse the same pipeline for scanning electron microscope (SEM) images,
which are grayscale and typically 16-bit per channel.

- **Grayscale input**: pipeline currently assumes BGRA float. Need to handle
  CV_16UC1 (gray) and CV_16UC2 (gray+alpha) on load, convert to float internally.
- **16-bit**: `imread(IMREAD_UNCHANGED)` loads 16-bit correctly; `convertTo(CV_32F,
  1.0/65535.0)` for normalisation. OKLab conversion assumes sRGB input — for SEM
  (linear, no colour) just use L channel directly (skip a/b, use intensity diff).
- **Gray+alpha TIFF**: OpenCV `imwrite` hard-rejects 2-channel images (assertion:
  channels must be 1, 3, or 4). Writing gray+alpha requires libtiff directly
  (already linked via tag_tiff). Alternatively store as 4-channel with R=G=B=gray.
- **MultiBandBlender**: accepts CV_16SC3 — for grayscale replicate the single channel
  3× before feeding, or investigate whether CV_16SC1 works.

## SmartBlend formula (Norel 2011)
> psychovisual error + Kolmogorov min-cut + ENBLEND + subpixel accuracy + pyramid with alpha
