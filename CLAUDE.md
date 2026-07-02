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
Windows: `conanfile.txt` + `conan_profile_msvc2026` (MSVC 195, cppstd 23, Ninja).
Unverified: whether conan's generated OpenCVConfig honors
`find_package(OpenCV COMPONENTS …)` and defines `OpenCV_LIBS` — if configure
fails there, link `opencv::opencv_core` etc. directly.

## CLI
```
pano-blend img1.tif img2.tif [img3.tif ...] -o out.tif
pano-blend img1.tif img2.tif -SeamMaskOnly mask.tif   # label map only, no blending
pano-blend img1.tif img2.tif -o out.tif -SeamVerbose   # debug TIFFs
```
- Image positions read from TIFF tags (XPOSITION/YPOSITION), or `-xoff`/`-yoff`
  (may be negative; `tiffio::kNoPos` = INT_MIN is the "not specified" sentinel)
- `-f WxH+X+Y` — canvas geometry; offsets may be negative (`-12` or `+-12` style)
- `-SeamMaskOnly F` — write label map (0=uncovered, 1..N=image index) and exit
- `-SeamVerbose` — per-pair error/seam/seam_viz TIFFs (numbered for 3+ images),
  plus `labelmap_viz.tif` (label map colorized via a golden-angle OkLCh palette)
  and `labelmap_legend.tif` (swatch + input filename key for that palette)
- `-w`, `-v` accepted and ignored (enblend compat)
- SmartBlend flags (`-DER`, `-DEC`, `-MinSize`, etc.) accepted with warning

## Pipeline
| Step | Function | Description |
|---|---|---|
| 1 | `seam::computeError` | OKLab ΔE per pixel; computed only inside the pair's overlap rect, row-parallel; rest of canvas = `seam::kNoOverlap` sentinel |
| 2 | `seam::findSeam` | Coarse-to-fine BK min-cut seam (8× downsample + 64px band refinement); works crop-local on alpha channels only |
| 3 | `buildLabelMap` | Combine pairwise seams into single label map (0..N) |
| 4 | `blend::multiBandBlend` | N-image Laplacian pyramid composite via label map; feeds per-image bounding rects, not full canvas |

### Seam-finding invariants (learned the hard way)
- **No pixel overlap ≠ all-zeros mask**: when bounding boxes intersect but opaque
  pixels don't (warped images, transparent corners), `findSeam` must return the
  coverage split — an all-zero mask makes `buildLabelMap` hand image j's territory
  to image i and punches holes in the output.
- **Crop expansion = kScale (8), not 1**: the coarse pass downsamples the crop, so
  a 1px single-image border vanishes at 1/8 scale → no hard T-weights → the coarse
  cut is unconstrained (uniform mask, empty band, `maxFlow` assert on empty graph).
- **Do not add a smoothness epsilon to edge capacities**: zero-capacity edges are
  skipped; where images are identical the seam position is irrelevant to the output.
  A `+1` prior was tried and reverted — it turns identical-content overlaps into
  BK's pathological case (1 s → 33 s on a 1200×3000 overlap).
- `GCGraph::maxFlow()` asserts on an edgeless graph; `graphCut` guards this and
  falls back to coarse mask / img1.

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
  - COMPRESSION_ADOBE_DEFLATE with configurable zlib level (1-9, default 1)
    + PREDICTOR_HORIZONTAL (predictor recovers most of the ratio on photos;
    level 6 cost seconds on big canvases)
  - EXTRASAMPLE_UNASSALPHA tag for 4-channel images
  - BGRA→RGBA channel reorder (OpenCV stores BGR internally)
- `readTiff()` reads TIFFTAG_XPOSITION/YPOSITION for canvas placement
- `readTiff()` rejects with a clear error what the scanline reader would
  silently misread: tiled TIFFs, planar-separate layout, non-uint sample formats

## Test data
- `test-data/p1.tif`, `p2.tif` — 405×240 8-bit RGBA (loaded as float internally);
  p2 starts at x=85
- `test-data/mask.tif`, `mask_viz.tif` — label map from 2-image seam
- Large test: 3× DSCF photos (5000×3000 each) → 9784×4396 canvas, ~26s
  (pre-optimization figure — re-measure)
- Perf baseline (2026-06, sandbox): 2× 5000×3000, 1200px noisy overlap:
  18.5 s → 14.2 s after crop-local seam/ROI blend/level-1 write
  (computeError 1033→331 ms, blend 3929→2235 ms, write 5221→4118 ms);
  remaining hotspot is the fine-pass BK itself (~5 s on dense-error overlap)

## OpenCV gotchas
- `imread` always delivers BGR/BGRA regardless of file format
- OKLch H field is in **degrees** (see `convLchToLab` in colors.cpp)
- `cv::detail::GCGraph` is in `<opencv2/imgproc/detail/gcgraph.hpp>`
- `cv::detail::MultiBandBlender` needs `find_package(OpenCV … stitching)`

## What's next

### Known limitation to address: all-pairs label map is incoherent in 3+ overlaps
`main.cpp` seams **every** bounding-box-overlapping pair (N(N-1)/2 = 10 for the
5-frame test) and merges the binary masks in `buildLabelMap`. Two problems:
- **Wasteful**: for a row panorama only spatially-adjacent pairs matter (~N-1);
  the rest are redundant seams. perf (2026-07, RelWithDebInfo, 5× X100V) shows
  `seam::graphCut` self ≈ 35% of CPU and it is the wall-clock hotspot — that cost
  is paid once per pair, so fewer pairs ≈ linear speedup on the seam phase.
- **Error-blind & order-dependent**: `buildLabelMap` keeps no ΔE. Priority is pure
  image index (lowest-index init, then pairs applied in fixed `(0,1)…(3,4)` order,
  later overriding). 2-image overlaps come out exact (the single seam *is* the
  answer), but 3+-image overlaps are resolved by an arbitrary index-ordered cascade
  of mutually-independent binary seams that need not meet consistently at the triple
  point — **not** the minimum-error partition.
- **Fix (both at once)**: switch to SmartBlend's **sequential pairwise** model —
  seam image k against the accumulated composite of 0..k-1 (N-1 seams, decisions
  always made against everything already placed, so no independent-pairwise masks
  to reconcile). This is also what the RE found (`08_FinalArchitecture.md`:
  "strictly pairwise sequential"). Requires reworking `buildLabelMap` + the N-way
  `multiBandBlend` into an accumulating composite; results become input-order
  dependent (as enblend's are). A cheaper interim step is pruning non-adjacent
  pairs by opaque-overlap area, but that only fixes the waste, not the incoherence,
  and is only safe when the "between" image always covers the non-adjacent overlap.

### Performance (next wins, in order)
1. **Band-only graph vertices**: fine-pass `graphCut` still allocates a vertex per
   crop pixel; an index-remap of band pixels would shrink the BK graph ~10×.
   This is the remaining hotspot (~5 s fine pass on a 1200×3000 dense overlap).
2. **Crop-based architecture**: `placeOnCanvas` still materializes a full-canvas
   CV_32FC4 (16 B/px) per image — the memory/scaling blocker for the SEM grid.
   Keep crops + offsets; do seam work in overlap-local coordinates.

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
