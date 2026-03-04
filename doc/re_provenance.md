# Reverse Engineering Provenance

What was taken from the SmartBlend binary RE vs. designed independently.

Reference binary: `smartblend.exe` (2007, Borland C++ 32-bit x86, image base `0x400000`).
RE documents: `/mnt/c/dev-c/blend/smartblend/0*.md`.

---

## From reverse engineering

### n-weight formula: `cap = delta_u² + delta_v²`

**Source:** `02_ComputeSeamError.md`, VA `0x407d20`, recovered from:
```asm
; delta_u in al, delta_v in cl
movzx eax, al        ; zero-extend delta_u to 32-bit
imul  eax, eax       ; delta_u²
movzx ecx, cl        ; zero-extend delta_v
imul  ecx, ecx       ; delta_v²
add   eax, ecx       ; cap = delta_u² + delta_v²
mov   [edge+0x0c], eax
```
This is exact. Without the RE we would likely have guessed `|delta_u - delta_v|`
or just `delta` (not squared, not summed at both endpoints).

### Only doubly-covered pixels get n-weight edges

**Source:** `02_ComputeSeamError.md`, inner loop:
```cpp
if (pix1.alpha1 == 0) goto next_col;
if (pix1.alpha2 == 0) goto next_col;
// ... and same checks for the neighbour before inserting the edge
```
Both endpoints must be covered by both images. Single-image pixels get no n-weight
edges between themselves — only at their boundary with the overlap (see below).

### kHard boundary edges (overlap ↔ single-image)

**Source:** `01_ComputeDirectPixelError.md`.
Single-image pixels are hard-pinned to their terminal with a large t-weight.
Our `kHard` boundary n-weights are the logical consequence: to prevent the min-cut
from escaping the overlap, boundary edges must also be high-capacity. This was
inferred from the graph topology described in the RE, not from a single instruction.

### Right + down edges only (not all 4-connected)

**Source:** `02_ComputeSeamError.md`, loop structure — only `(x, y+1)` and
`(x+1, y)` neighbours are visited per pixel. The reverse edges are added
symmetrically (the BK graph is undirected via paired forward/reverse arc objects),
so the effective graph is 4-connected even though only two directions are iterated.

### Graph covers the overlap bounding box only

**Source:** `02_ComputeSeamError.md` — the iterator is initialised over
`OverlapRect` (`x_left`, `y_top`, `x_right`, `y_bottom`), not the full canvas.
Our implementation allocates a full-canvas node array for simplicity (harmless —
nodes outside the overlap are isolated or pinned).

### Number of pyramid bands = 5

**Source:** Russian SmartBlend description page (archived 2010), as reported by
Grok translation. Consistent with `cv::detail::MultiBandBlender` default.

---

## Designed independently (not from RE)

### OKLab as the colour space for delta

The RE confirms `delta` is a pre-computed uint8 "difference signal" per pixel
(`02_ComputeSeamError.md` §5.1: *"luminance or a bandpass/gradient response"*).
What exactly is computed is unresolved — it requires RE of `buildComposite`
(vtable[11]) and `seekTo` (VA `0x407c10`, 63 insns, still pending).

We chose OKLab because it is perceptually uniform and available via the
`color::okLabFromRgb` library already in the project.

### Laplacian pyramid via `cv::detail::MultiBandBlender`

SmartBlend's `compositeBlend` (vtable[13]) is partially RE'd but its internals
are not fully traced. We use OpenCV's existing MultiBandBlender which implements
the same Burt-Adelson (1983) algorithm described by Norel in his 2011 post.

### kHard = 1e6

SmartBlend uses a concrete large integer for its t-weights (recovered from
`01_ComputeDirectPixelError.md` as `±268,435,456` = `0x10000000`). We use `1e6f`
which is sufficient for our float graph and avoids overflow.

### Boundary n-weight = kHard (vs. SmartBlend's behaviour)

SmartBlend only adds n-weight edges between doubly-covered pixels; it never adds
an edge between a singly-covered pixel and an overlap pixel (the `goto next_col`
skips that case entirely). Our boundary `kHard` edges are an addition to correct
the graph connectivity in OpenCV's GCGraph — without them, the source tree cannot
grow into the overlap from the left, causing the trivial seam bug (all overlap
assigned to image2). This discrepancy may be due to differences between SmartBlend's
custom BK implementation and OpenCV's `cv::detail::GCGraph`.

---

## Open RE targets

| Target | VA | Status | Relevance |
|---|---|---|---|
| `buildComposite` / `seekTo` | `0x407c10` | Not traced | Defines the `delta` channel |
| `ComputeDirectPixelError` detail | `0x4076b0` | Partial | T-weight exact values |
| `compositeBlend` | vtable[13] | Not traced | Final pixel write-back / alpha blend |
| Subpixel accuracy | unknown | Not found | Norel's 4th ingredient |
