# pano-blend Algorithm

Implements the SmartBlend (Norel, 2007) pipeline in modern C++/OpenCV.

> psychovisual error + Kolmogorov min-cut + ENBLEND + subpixel accuracy + pyramid with alpha
> — Michael Norel, 2011

The first three ingredients are implemented. The last two are open research targets.

---

## Pipeline

```
p1.tif, p2.tif (BGRA float [0,1])
        │
        ▼
┌─────────────────────┐
│  computeError       │  OKLab ΔE per overlap pixel          → error.tif
└─────────────────────┘
        │
        ▼
┌─────────────────────┐
│  findSeam           │  Boykov-Kolmogorov min-cut            → seam.tif
└─────────────────────┘
        │
        ▼
┌─────────────────────┐
│  multiBandBlend     │  Laplacian pyramid composite          → blend.tif
└─────────────────────┘
```

---

## Step 1 — computeError

For every pixel covered by **both** images, compute the perceptual colour distance
in OKLab space:

```
ΔE(x,y) = √( ΔL² + Δa² + Δb² )
```

OKLab is used because its Euclidean distance correlates with perceived colour
difference better than sRGB or CIE Lab for typical photographic content.

Pixels covered by only one image (or neither) receive a large sentinel value
(`kNoOverlap = 1e6`) so they can be excluded cleanly from downstream statistics.

---

## Step 2 — findSeam (Boykov-Kolmogorov graph cut)

Finds the seam through the overlap region along which cutting the composite would
be least visible.

### Graph structure

One node per pixel in the canvas. Three types of nodes:

| Situation | `in1` | `in2` | Role |
|---|---|---|---|
| Only in image 1 | ✓ | ✗ | Hard-pinned to **source** (S) |
| Only in image 2 | ✗ | ✓ | Hard-pinned to **sink** (T) |
| In both (overlap) | ✓ | ✓ | Free — seam can pass through |
| In neither | ✗ | ✗ | Isolated — ignored |

### T-weights (terminal edges)

`addTermWeights(idx, src_cap, snk_cap)`:

- `src_cap` = capacity of the edge **from source → pixel**.
  A high value forces the pixel into the source (image1) partition.
- `snk_cap` = capacity of the edge **from pixel → sink**.
  A high value forces the pixel into the sink (image2) partition.

```
in1 && !in2  →  addTermWeights(idx, kHard, 0)     // image1-only: source side
!in1 && in2  →  addTermWeights(idx, 0,     kHard)  // image2-only: sink side
in1 && in2   →  (no terminal edge — free node)
```

`kHard = 1e6` — large enough to prevent the min-cut from ever severing these edges.

### N-weights (neighbour edges)

For each pixel **u** = `(y, x)` and each of its two neighbours **v** = `(y, y+1)`
and `(x+1, y)`:

| u | v | Edge capacity |
|---|---|---|
| overlap | overlap | `eu² + ev²` |
| overlap | single-image | `kHard` |
| single-image | overlap | `kHard` |
| single-image | single-image | *(no edge)* |

Variable meanings:

| Variable | Meaning |
|---|---|
| `in1` | pixel **u** is present in image 1 (alpha > 0.5) |
| `in2` | pixel **u** is present in image 2 (alpha > 0.5) |
| `overlap` | `in1 && in2` — pixel **u** covered by both images |
| `eu` | OKLab ΔE at pixel **u** (0 if not in overlap) |
| `n1` | neighbour **v** is present in image 1 |
| `n2` | neighbour **v** is present in image 2 |
| `n_overlap` | `n1 && n2` — neighbour **v** covered by both images |
| `ev` | OKLab ΔE at neighbour **v** (0 if not in overlap) |

**Why eu² + ev²?**
The capacity of the edge between u and v encodes how costly it would be for the
min-cut to sever that edge (i.e. for the seam to pass between u and v).
A high capacity means "don't cut here". Squaring makes the cost sensitive to large
differences and relatively tolerant of small ones.
This is the exact formula recovered from SmartBlend's `ComputeSeamError` (see
`re_provenance.md`).

**Why kHard at overlap/single-image boundaries?**
Single-image pixels are already hard-pinned to their terminal. An n-weight edge
connecting a single-image pixel to an overlap pixel with capacity `kHard` prevents
the min-cut from leaving the overlap region: severing that boundary edge would cost
`kHard`, which is always more expensive than any path through the overlap interior.
This is what forces the seam to stay inside the doubly-covered zone.

### Min-cut result

After `maxFlow()`:

```
inSourceSegment(idx) == true   →  pixel assigned to image 1  (mask = 0)
inSourceSegment(idx) == false  →  pixel assigned to image 2  (mask = 255)
```

The seam is the boundary between the two partitions, threading through the overlap
region along the path of minimum colour disagreement.

---

## Step 3 — multiBandBlend

Uses `cv::detail::MultiBandBlender` (OpenCV stitching module, Burt-Adelson 1983).

Territory masks are derived from the seam mask:

```
mask1 = (seam_mask == 0)   AND  f1 present   →  image1 territory
mask2 = (seam_mask == 255) AND  f2 present   →  image2 territory
```

The blender builds Laplacian pyramids for both images and blends level by level
using the Gaussian pyramid of the territory boundary as a smooth weight. At coarse
pyramid levels the transition zone is wide (smooth colour blending); at fine levels
it is narrow (sharp texture blending). This makes the seam invisible across all
spatial frequencies.

Number of bands: 5 (default, matches SmartBlend — confirmed from the Russian
SmartBlend description page, 2010).
