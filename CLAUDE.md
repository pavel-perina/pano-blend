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

## What's next (SmartBlend parity)
1. **High-pass delta** — SmartBlend uses Gaussian-blurred colour difference as seam
   cost (HiPassLevel=4), not raw OKLab distance. Seam prefers flat zones, avoids edges.
2. **RE: `seekTo` (0x407c10)** — likely contains subpixel / pyramid-with-alpha blend
3. **RE: outer loop (0x405520)** — per-row pixel write-back after seam

## SmartBlend formula (Norel 2011)
> psychovisual error + Kolmogorov min-cut + ENBLEND + subpixel accuracy + pyramid with alpha
