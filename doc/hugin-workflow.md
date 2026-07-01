# Using pano-blend with Hugin / nona

`pano-blend` is a **blender, not a stitcher**. It does not warp images or solve
for camera geometry — it takes already-warped, correctly-positioned image layers
and composites them along an optimal seam. The usual upstream that produces those
layers is [Hugin](https://hugin.sourceforge.io/) and its remapper `nona`, so
`blend` slots into the same place [enblend](https://enblend.sourceforge.net/)
occupies in a Hugin workflow.

```
project.pto ──nona──▶ warped TIFF layers ──blend──▶ panorama.tif
 (align in         (per-image, cropped,       (seam + multi-band
  Hugin GUI)        with position tags)         composite)
```

---

## 1. Align in Hugin

Create and optimise your panorama project as usual in the Hugin GUI (add images,
find control points, optimise, set crop/canvas). Save it as `project.pto`. Nothing
in this step is specific to `pano-blend`.

## 2. Remap to layers with nona

Generate one warped TIFF **per input image** (the multi-file mode enblend also
consumes):

```sh
nona -o remapped_ -m TIFF_m project.pto
# produces remapped_0000.tif, remapped_0001.tif, ...
```

Each layer is a cropped RGBA TIFF carrying `XPOSITION`/`YPOSITION` tags that place
it on the full panorama canvas. `blend` reads those tags directly (position in
pixels = `XPOSITION × XRESOLUTION`, the standard nona/enblend convention), so you
do **not** need to pass `-xoff`/`-yoff`.

> Use `TIFF_m` (separate files), **not** `TIFF_multilayer`. `blend` reads the
> first page of each file it is given; a single multi-page TIFF would only expose
> its first layer.

## 3. Blend

```sh
blend remapped_*.tif -o panorama.tif
```

`blend` finds seams pairwise, builds a combined label map, and composites all
layers with a Laplacian-pyramid (multi-band) blend. To inspect the seam decision
without blending:

```sh
blend remapped_*.tif -SeamMaskOnly labels.tif   # 0 = uncovered, 1..N = image index
```

### Worked example (5-image panorama)

A real 5-shot horizontal pano (Fujifilm X100V, 6232×4156 px each) run through this
exact path produced an 11288×5153 canvas. `blend` read all five position tags directly
from nona's output — no `-xoff`/`-yoff` needed:

```
p_0000.tif at (2446,1318)   p_0001.tif at (627,1318)   p_0002.tif at (121,1318)
p_0003.tif at (4641,1318)   p_0004.tif at (5268,1318)
```

10 overlapping pairs were seamed and blended in one pass. Note that pairs can share
a large *bounding-box* overlap without sharing many opaque pixels (all layers span
the same vertical band here); `blend` resolves those as a coverage split rather than
punching holes. On this scene the dominant cost is the fine-pass seam search — the
multi-band blend and per-pair `findSeam` are the heaviest steps, so expect the run
to be seam-bound on wide overlaps.

---

## Input requirements

`nona`'s default `TIFF_m` output satisfies all of these, but if you feed `blend`
TIFFs from another source, note that `readTiff` rejects (with a clear error) what
its scanline reader would otherwise misread:

| Requirement | Accepted |
|---|---|
| Bit depth | 8 or 16-bit **unsigned integer** (not float) |
| Layout | strip-based (**tiled TIFFs rejected**) |
| Channel layout | contiguous / interleaved (**planar-separate rejected**) |
| Photometric | RGB or grayscale (MINISBLACK / MINISWHITE); alpha optional |
| Position | `XPOSITION`/`YPOSITION` tags, or `-xoff`/`-yoff` on the command line |

If a layer lacks position tags, override per-image on the command line — the
offset applies to the image that **precedes** it:

```sh
blend layer0.tif layer1.tif -xoff 850 -yoff 0 -o panorama.tif
```

---

## Substituting for enblend inside Hugin (untested)

Because `blend` mirrors enblend's core CLI — `-o output.tif`, positional input
TIFFs, and `-f WxH+X+Y` canvas geometry — it can in principle be used as Hugin's
blender by pointing the stitcher at the `blend` binary instead of `enblend`.

**This path has not yet been verified through the Hugin GUI.** Before relying on
it, be aware of the flag handling:

| Flag | Behaviour in `blend` |
|---|---|
| `-o` / `--output` | honored (output path) |
| `-f WxH+X+Y` | honored (force canvas geometry; negative offsets ok) |
| `-w`, `-v` | accepted and **ignored** (enblend compat) |
| `-DER`, `-DEC`, `-MinSize`, `-HiPassLevel`, … | accepted with a warning, **no effect** |

Any enblend option not in the honored set is either ignored or warned about, so
the blend will run but will not act on those settings. Treat the result as
"enblend-compatible enough to invoke," not "feature-equivalent to enblend," until
a full GUI round-trip has been confirmed.
