#!/usr/bin/env python3
# Colorize a label map TIFF into a palette (indexed) TIFF.
#
# Usage: colorize_mask.py input_mask.tif output.tif
#
# Label 0 (no image) is transparent black.
# Labels 1..N get distinct colours from an OkLrCh golden-angle palette.
#
# /// script
# dependencies = [ "tifffile", "numpy", "coloraide" ]
# ///

import sys
import numpy as np
import tifffile
from coloraide import Color
from coloraide.spaces.oklrch import OkLrCh
from coloraide.spaces.oklrab import Oklrab

Color.register(OkLrCh())
Color.register(Oklrab())

GOLDEN_ANGLE_DEG = 180 * (3 - np.sqrt(5))  # ~137.508°


def make_palette(n: int) -> np.ndarray:
    """Generate an Nx3 uint8 RGB palette using OkLrCh golden-angle spacing."""
    palette = np.zeros((n, 3), dtype=np.uint8)
    hue = 0.0
    for i in range(n):
        c = Color("oklrch", [0.66, 0.12, hue]).convert("srgb")
        r, g, b = c.get("red"), c.get("green"), c.get("blue")
        palette[i] = [
            int(np.clip(r * 255, 0, 255)),
            int(np.clip(g * 255, 0, 255)),
            int(np.clip(b * 255, 0, 255)),
        ]
        hue = (hue + GOLDEN_ANGLE_DEG) % 360.0
    return palette


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} input_mask.tif output.tif", file=sys.stderr)
        sys.exit(1)

    mask = tifffile.imread(sys.argv[1])
    if mask.ndim != 2:
        print(f"Error: expected 2D image, got shape {mask.shape}", file=sys.stderr)
        sys.exit(1)

    max_label = int(mask.max())
    print(f"Mask: {mask.shape[1]}x{mask.shape[0]}, labels 0..{max_label}")

    # Build RGBA palette: label 0 = transparent, labels 1..N = colours
    palette = make_palette(max_label)
    rgba_palette = np.zeros((256, 4), dtype=np.uint8)
    for i in range(max_label):
        rgba_palette[i + 1, :3] = palette[i]
        rgba_palette[i + 1, 3] = 255

    # Apply palette → RGBA image
    rgba = rgba_palette[mask]

    tifffile.imwrite(sys.argv[2], rgba, compression="deflate")
    print(f"Written: {sys.argv[2]}")


if __name__ == "__main__":
    main()
