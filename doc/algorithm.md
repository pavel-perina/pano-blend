# pano-blend Algorithm

Implements the SmartBlend (Norel, 2007) pipeline in modern C++/OpenCV, extended
to N images.

> psychovisual error + Kolmogorov min-cut + ENBLEND + subpixel accuracy + pyramid with alpha
> — Michael Norel, 2011

The first three ingredients are implemented. The last two are open research targets.

---

## Pipeline

For N input images, seams are found **per overlapping pair**, merged into a single
**label map**, and the label map drives one N-image pyramid composite.

```
img1..imgN (BGRA float [0,1], positioned on a shared canvas)
        │
        ▼   for each overlapping pair (i, j)
┌─────────────────────┐
│  computeError       │  OKLab ΔE per overlap pixel          → error_i_j.tif
└─────────────────────┘
        │
        ▼
┌─────────────────────┐
│  findSeam           │  coarse-to-fine BK min-cut           → seam_i_j.tif
└─────────────────────┘
        │
        ▼   combine all pairwise seams
┌─────────────────────┐
│  buildLabelMap      │  0 = uncovered, 1..N = winning image → labelmap_viz.tif
└─────────────────────┘
        │
        ▼
┌─────────────────────┐
│  multiBandBlend     │  N-image Laplacian pyramid composite → out.tif
└─────────────────────┘
```

(The `*.tif` debug outputs are written only under `-SeamVerbose`.)

---

## Step 1 — computeError

For every pixel covered by **both** images of a pair, compute the perceptual
colour distance in OKLab space:

```
ΔE(x,y) = √( ΔL² + Δa² + Δb² )
```

OKLab is used because its Euclidean distance correlates with perceived colour
difference better than sRGB or CIE Lab for typical photographic content. It is
computed only inside the pair's overlap rectangle (row-parallel); everywhere else
the error map holds a sentinel (`kNoOverlap = 1e6`) so single-image pixels are
excluded cleanly from downstream statistics.

---

## Step 2 — findSeam (coarse-to-fine Boykov–Kolmogorov graph cut)

Finds the seam through the overlap along which cutting the composite would be
least visible, as a min-cut of a graph whose edge capacities encode "how costly
it would be to cut here".

### Crop-local, not full-canvas

`findSeam` first scans for the bounding box of the **doubly-opaque** region
(both alphas > 0.5) and works only inside that crop (expanded by the coarse
downsample factor). If the two images' bounding boxes intersect but no opaque
pixels overlap — common with warped frames and transparent corners — there is no
seam to find, and it returns the **coverage split** so each image keeps its own
territory. (Returning an all-zero mask here would hand image *j*'s pixels to
image *i* and punch holes in the output.)

### Coarse-to-fine

A single full-resolution graph cut over a large overlap is expensive, so the cut
runs in two passes:

1. **Coarse pass** — alphas and error are downsampled by `kScale = 8`; BK runs on
   the small graph to get an approximate seam. Even with deep BK search trees the
   graph is tiny, so this is fast.
2. **Fine pass** — the coarse seam is upsampled, and a band of `kBandRadius = 64`
   px around it is refined at full resolution. Only band pixels become free graph
   nodes; pixels outside the band are pinned to the coarse result. This keeps the
   full-res graph ~10× smaller than the whole crop.

### Graph structure

Three kinds of node, by alpha coverage of the two images:

| Situation | `in1` | `in2` | Role |
|---|---|---|---|
| Only in image 1 | ✓ | ✗ | Hard-pinned to **source** (S) |
| Only in image 2 | ✗ | ✓ | Hard-pinned to **sink** (T) |
| In both (overlap) | ✓ | ✓ | Free — seam can pass through |
| In neither | ✗ | ✗ | Isolated — ignored |

**T-weights** (terminal edges), `addTermWeights(idx, src_cap, snk_cap)`:

```
in1 && !in2  →  addTermWeights(idx, kHard, 0)     // image1-only: source side
!in1 && in2  →  addTermWeights(idx, 0,     kHard)  // image2-only: sink side
in1 && in2   →  (no terminal edge — free node)
```

`kHard = 1e6` — large enough that the min-cut never severs these edges. In the
fine pass, band-edge pixels also get a `kHard` T-weight toward the side the
coarse mask assigned, anchoring the refinement to the coarse decision.

**N-weights** (neighbour edges), for each pixel **u** and its right/down
neighbours **v** = `(y, x+1)` and `(y+1, x)`:

| u | v | Edge capacity |
|---|---|---|
| overlap | overlap | `eu² + ev²` |
| overlap | single-image | `kHard` |
| single-image | overlap | `kHard` |
| single-image | single-image | *(no edge)* |

where `eu`, `ev` are the OKLab ΔE at **u** and **v** (0 outside the overlap).

**Why `eu² + ev²`?** The edge capacity is the cost of the seam passing between
**u** and **v**; a high capacity means "don't cut here". Squaring makes the cost
sensitive to large colour disagreements and tolerant of small ones. This is the
exact formula recovered from SmartBlend's `ComputeSeamError` (see
`re_provenance.md`) — the RE was needed to get it right; a natural guess would
have been `|eu − ev|`.

**Why `kHard` at overlap/single-image boundaries?** Single-image pixels are
already pinned to their terminal. A `kHard` boundary edge prevents the min-cut
from escaping the overlap: leaving it would cost `kHard`, always dearer than any
path through the overlap interior. This forces the seam to stay inside the
doubly-covered zone.

### Min-cut result

After `maxFlow()`:

```
inSourceSegment(idx) == true   →  pixel assigned to image 1  (mask = 0)
inSourceSegment(idx) == false  →  pixel assigned to image 2  (mask = 255)
```

The returned mask is crop-sized (0 = image *i*, 255 = image *j*); single-image
pixels take their own side, so it expands back to the canvas cleanly.
(`GCGraph::maxFlow()` asserts on an edgeless graph, e.g. isolated overlap pixels;
`graphCut` guards this and falls back to the coarse mask.)

---

## Step 3 — buildLabelMap (N images)

The pairwise seams are binary masks; `buildLabelMap` merges them into one label
map (`CV_8UC1`: 0 = uncovered, 1..N = winning image index):

1. **Init** — each pixel is assigned to the *lowest-index* image that covers it.
2. **Apply seams** — the pairwise seams are applied in fixed pair order; each seam
   overwrites a pixel *only if its current label is one of that pair's two images*,
   setting it to image *i* or *j* per the mask. Later pairs override earlier ones.

**Two-image overlaps come out exact** — only images *i, j* cover the pixel, so the
single `(i, j)` seam *is* the answer. **Overlaps of three or more images are
resolved by an index-ordered cascade** of mutually-independent binary seams that
need not meet consistently at the triple point — this is *not* guaranteed to be
the minimum-error partition, and no ΔE is retained here to arbitrate. This is a
known limitation; the intended fix is SmartBlend's sequential-pairwise model
(seam each image against the accumulated composite). See `CLAUDE.md`.

---

## Step 4 — multiBandBlend

`blend::multiBandBlend` composites all N images through `cv::detail::MultiBandBlender`
(OpenCV stitching module, Burt–Adelson 1983), driven by the label map.

For each image *i*, its **territory** is `label_map == i` (∧ the image is present).
The image is fed to the blender over its **bounding rect only** — each feed builds
a Laplacian pyramid over the fed region, so full-canvas feeds would cost N× the
canvas. Transparent pixels inside that rect are **hole-filled** with the nearest
present colour before feeding: otherwise, at coarse pyramid levels the black of
an image's transparent edge bleeds across the territory boundary and leaves a dark
halo where a seam meets that edge.

The blender then blends level by level using the Gaussian pyramid of the territory
boundary as a smooth weight: at coarse levels the transition zone is wide (smooth
colour blending), at fine levels it is narrow (sharp texture blending), so the
seam is invisible across all spatial frequencies. Number of bands: 5 (default,
matches SmartBlend). The result is `CV_8UC4` BGRA.
