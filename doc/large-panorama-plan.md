# Large-Panorama / Tiled Blending — Design & Plan

**Status:** design converged in discussion (2026-07). **Phase 1 implemented
2026-07** (`src/labelmap.h/.cpp` + `tests/test_labelmap.cpp`); Phases 0, 2–4
pending. Supersedes the *"all-pairs label map is incoherent"* limitation and
the loose *"SEM pipeline"* sketch in `CLAUDE.md`.

---

## Core model — one algorithm, one order, two executors

- **Algorithm:** sequential accumulate — place image 0, then cut image *k* against
  the accumulated coverage of 0..k−1, for k = 1..N−1. **N−1 cuts**, not N(N−1)/2.
- **The label map is the *memo* of that process.** You do not need to composite
  pixels to keep ownership: record each step's binary cut into a running label map.
  - init: `label = 0` wherever image 0 is opaque
  - step k: graph-cut image k vs current coverage in their overlap → winners get
    `label = k`; losers **keep their existing label**
  - after N−1 steps: a coherent global label map (0 = uncovered, 1..N = image index)
- **No error accumulation.** pano-blend is a *blender*, not a stitcher: Hugin/nona
  already solved geometry (bundle adjustment) and, if run, photometry (exposure/
  vignetting). The blend estimates nothing global and never re-exposes anything, so
  **order cannot cause drift.** Order affects *only* the partition at multi-image
  overlap points (triple/quad corners) — and with 5–15% overlaps those are tiny
  `overlap²` slivers. Everywhere only two images meet, the cut is the plain pairwise
  min-cut, identical regardless of order.

### Ordering: maximum-overlap spanning tree from center

Freeze **one** global order that is simultaneously:
- **deterministic** — required so tiled execution is consistent across output tiles
- **connected** — each newcomer must overlap the placed blob so "cut against
  composite" is well-defined
- **best-supported-first** — place tiles with the strongest available seam support early

That order = **Prim's algorithm on the overlap graph, rooted at the center tile,
edge weight = shared overlap area.** At each step add the unplaced tile with the
largest-area overlap against the placed set.

- Edge weight approximated by **bounding-box intersection area** (cheap; use a
  spatial index / grid hash to avoid all-pairs, or grid indices when structured).
  - Exact for axis-aligned, fully-opaque SEM tiles.
  - Slight *overestimate* for nona-warped photos (transparent corners) — but bbox
    is only the *ordering* weight, never a seam decision, so the error is contained
    to a low-stakes ordering choice. Real masks are still used for the cut.
- Area beats the alternatives: *neighbor count* biases growth toward junctions
  (the "two overlaps wins" pathology); *geometric center-distance* ignores how much
  two tiles actually share. Area = seam support.
- Growth shape (cross / diamond / sideways) self-organizes and does not affect the
  output — it is only the visitation pattern.

**Ordering is a pluggable strategy (interface), not a hard-coded step.** The default
implementation is Prim max-overlap-from-center; a capture pipeline that already knows
a sensible visitation order can supply its own. Any strategy is acceptable as long as
it is deterministic + connected. **This is not critical and need not be perfect** —
because there is no drift and overlaps are small, the order only nudges tiny triple-
point slivers. Phase 1 may start with any trivial deterministic order (index / spatial
raster) and add the center-out heuristic later.

---

## Two executors of the same label map

- **Small (fits RAM):** in-RAM sequential accumulate → composite directly.
- **Large (does not fit):** two passes —
  - **Pass 1 — global, cheap, mask-only:** build the label map via the ordered
    sequential cut. Uses coverage masks + original pixels only (no rendered
    composite); can be run downsampled.
  - **Pass 2 — tiled, heavy:** apron-tiled multi-band blend that *consumes the
    frozen label map*. Order-independent to execute (order was baked into the map).

### Two independent boundary hazards in Pass 2 — both must be fixed

1. **Pyramid low-frequency:** independent *touching* tiles reconstruct low
   frequencies from different boundary conditions → a visible seam **on the tile
   grid**. Fix: the *processing* window overlaps (read tile + apron of real neighbor
   pixels, build pyramid, write only the center); the *written* window abuts.
   - apron ≈ `kernel_radius · 2^levels` (~500 px at 8 levels, 5-tap)
   - **align tile origins to a multiple of `2^levels`** so all tiles share one global
     downsample lattice (kills the phase-misalignment variant)
   - knob: if `levels` is large the apron rivals the tile → cap `levels` (bounds
     apron, narrows feather), or compute the few coarsest levels globally at reduced
     res and let tiles supply only the fine bands.
2. **Seam order:** the order-dependent label map must be **global and frozen before
   tiling** — Pass 1 guarantees this. (Apron fixes hazard 1; global map fixes hazard 2.
   They are independent.)

---

## Output

- Compute into temporary large output tiles (e.g. 8192², 192 px apron).
- Assemble into **BigTIFF** (`TIFFOpen(..., "w8")`), **tiled** 512², with
  **reduced-resolution overview IFDs** (`FILETYPE_REDUCEDIMAGE`) → pyramidal TIFF.
  The coarse pyramid levels come for free if Pass 2 computes coarse levels globally.
- Optional: **Hilbert order for the tile *write* traversal** (IO/cache locality) —
  orthogonal to the image accumulation order.

---

## Phased TODO

### Phase 0 — Prerequisites — DONE
- [x] Crop-based architecture (2026-07): `placeOnCanvas` removed; crops +
      placement rects flow through `labelmap::accumulate` and
      `blend::multiBandBlend`. Remaining full-canvas buffers: the mosaic
      (CV_32FC4, one), the label map (CV_16UC1), and MultiBandBlender's
      internal destination pyramids — the latter fall to Phase 3.
- [x] Grayscale + 16-bit load path: `readTiff` loads 8/16-bit gray, gray+alpha,
      RGB(A) → CV_32FC4 with a grayscale flag; `computeError` uses intensity
      diff when all inputs are grayscale. (Was already in place.)

### Phase 1 — Sequential label map (fixes existing 3+-overlap incoherence) — DONE
- [x] Replace `buildLabelMap`'s all-pairs binary-mask merge with an **ordered
      sequential accumulate** (`labelmap::accumulate`, cut against a hard-cut
      mosaic of placed originals).
- [x] Ordering: `labelmap::placementOrder` — Prim max-overlap spanning tree from
      the most central image (bbox-approx edge weights, int64 areas), deterministic,
      disconnected components restart from the center. Swappable: `accumulate`
      takes any frozen permutation.
- [x] Record each step's binary cut into the running label map (the "memo").
- [x] Verified: two-image output bit-identical to the pairwise implementation
      (unit test + binary diff on p1/p2 and a 5000×3000 pair); coverage
      invariants unit-tested at a triple overlap. Bonus: per-pair seam masks no
      longer stored, originals freed after placement (peak RSS 1814→1478 MB on
      the big pair).

### Phase 2 — Global / tiled decoupling
- [x] The label map is a standalone contract at the CLI: `-SeamMaskOnly` writes
      it, `-LoadLabelMap` blends from it (round-trip byte-identical, CLI-tested).
- [ ] Optional downsampled seam pass for very large canvases.

### Phase 3 — Tiled multi-band blend
- [ ] Apron-tiled multi-band executor consuming the global label map.
- [ ] Apron sizing `kernel_radius · 2^levels`; origin-align to lattice; write center only.
- [ ] `levels` cap knob (bounds apron) / optional global coarse levels.

### Phase 4 — Pyramidal BigTIFF output
- [ ] BigTIFF (`w8`), tiled 512², overview IFDs.
- [ ] Temporary large output tile → 512² storage-tile slicing.
- [ ] (opt) Hilbert write traversal.

---

## Open decisions

1. **"Closest-to-overlap-center" label** — two distinct readings; decide per mode.
   *Not* a Pass-2 concern (Pass 2 consumes the frozen label; the pyramid derives the
   feather from the hard mask):
   - **(a) DER/DEC bias in Pass 1** — a distance-to-overlap-center term added to the
     graph-cut cost, pulling the seam off tile edges toward the overlap center so the
     feather has room on both sides. Keeps the *optimal* graph-cut seam, just nudged.
     (This is the SmartBlend parity feature already listed in CLAUDE.md.)
   - **(b) distance/Voronoi label replacing graph-cut** for the giant SEM grid — fast,
     suboptimal seams, acceptable because SEM content is uniform. A speed/quality mode,
     not the default.
2. **bbox vs opaque-area for ordering** — bbox is the default; add a cheap opaque-area
   refinement only if warped-photo ordering proves visibly wrong (low risk — ordering
   is low-stakes and never decides a seam).
3. **Small/large mode switch** — automatic by canvas byte budget, or an explicit flag.

4. **Label-island cleanup — deferred (2026-07).** Where the error quantizes to
   zero (<1/255) the min-cut is degenerate and BK leaves arbitrary label
   enclaves ("islands"), typically in flat sky; with JPEG sources the region is
   additionally speckled with faint quantization-noise anchors. Decision:
   leave them. They are invisible in output (content identical by definition),
   don't interact with tiling (the map is frozen globally; apron is
   label-agnostic), and deflate absorbs the speckle. A boundary-anchored
   flood-fill post-pass would only merge fully-zero-cost enclaves and would
   miss the noise-anchored ones anyway. Revisit only if Pass-1 map compression
   or seam-perimeter cost measurably hurts at gigapixel scale.

## Non-goals / out of scope

- **De-ghosting / transient removal** (a car that drove through, an accidental
  duplicate frame) — a *content* decision, not a seam/blend problem. Graph-cut +
  multi-band will not vote out a moving object; that belongs to a separate pass or
  manual masks.
- **Global multi-label α-expansion** (the true order-independent optimum) — deliberately
  traded away: it is memory-hungry and does not tile. Sequential accumulate is the
  greedy, streaming-, tile-friendly approximation.
