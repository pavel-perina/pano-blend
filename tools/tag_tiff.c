/*
 * tag_tiff — inject canvas-position TIFF tags in-place.
 *
 * Adds XPOSITION/YPOSITION (rational, pixel units) and the Pixar
 * IMAGEFULLWIDTH/IMAGEFULLLENGTH tags to an existing TIFF without touching
 * image data.  Uses TIFFRewriteDirectory() which rewrites only the IFD.
 *
 * Tags written:
 *   286  XPOSITION          (RATIONAL) — x pixel offset in full canvas
 *   287  YPOSITION          (RATIONAL) — y pixel offset in full canvas
 *   296  RESOLUTIONUNIT     RESUNIT_NONE (1) so XPOS == pixels directly
 *   282  XRESOLUTION        1.0
 *   283  YRESOLUTION        1.0
 *   33300 PIXAR_IMAGEFULLWIDTH  — full panorama width  in pixels
 *   33301 PIXAR_IMAGEFULLLENGTH — full panorama height in pixels
 *
 * Usage:
 *   tag_tiff <file.tif> <xpos> <ypos> <full_width> <full_height>
 *
 * Example (p2 is a 320x240 crop placed at x=85 in a 405x240 panorama):
 *   tag_tiff p2.tif 85 0 405 240
 */

#include <tiffio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Register the Pixar private tags so libtiff doesn't warn about them. */
static void register_pixar_tags(TIFF* tif) {
    static const TIFFFieldInfo info[] = {
        { 33300, 1, 1, TIFF_LONG, FIELD_CUSTOM, 1, 0, "PixarImageFullWidth"  },
        { 33301, 1, 1, TIFF_LONG, FIELD_CUSTOM, 1, 0, "PixarImageFullLength" },
    };
    TIFFMergeFieldInfo(tif, info, 2);
}

static void print_tags(TIFF* tif, const char* path) {
    float  xpos = 0, ypos = 0, xres = 1, yres = 1;
    uint16_t ru = 0;
    uint32_t fw = 0, fh = 0, w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,     &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH,    &h);
    TIFFGetField(tif, TIFFTAG_XPOSITION,      &xpos);
    TIFFGetField(tif, TIFFTAG_YPOSITION,      &ypos);
    TIFFGetField(tif, TIFFTAG_XRESOLUTION,    &xres);
    TIFFGetField(tif, TIFFTAG_YRESOLUTION,    &yres);
    TIFFGetField(tif, TIFFTAG_RESOLUTIONUNIT, &ru);
    TIFFGetField(tif, 33300, &fw);
    TIFFGetField(tif, 33301, &fh);
    printf("%s\n", path);
    printf("  image:  %ux%u px\n", w, h);
    printf("  canvas: %ux%u px\n", fw, fh);
    printf("  offset: x=%.0f y=%.0f  (XPOS*XRES=%.0f)\n",
           (double)xpos, (double)ypos, (double)(xpos * xres));
}

int main(int argc, char** argv) {
    if (argc != 6) {
        fprintf(stderr,
            "usage: tag_tiff <file.tif> <xpos> <ypos> <full_width> <full_height>\n"
            "  All values are in pixels.  Modifies the file in place.\n");
        return 1;
    }

    const char* path   = argv[1];
    float       xpos   = (float)atof(argv[2]);
    float       ypos   = (float)atof(argv[3]);
    uint32_t    fw     = (uint32_t)atoi(argv[4]);
    uint32_t    fh     = (uint32_t)atoi(argv[5]);

    TIFF* tif = TIFFOpen(path, "r+");
    if (!tif) {
        fprintf(stderr, "error: cannot open '%s' for writing\n", path);
        return 1;
    }
    register_pixar_tags(tif);

    TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, (uint16_t)RESUNIT_NONE);
    TIFFSetField(tif, TIFFTAG_XRESOLUTION,    1.0f);
    TIFFSetField(tif, TIFFTAG_YRESOLUTION,    1.0f);
    TIFFSetField(tif, TIFFTAG_XPOSITION,      xpos);
    TIFFSetField(tif, TIFFTAG_YPOSITION,      ypos);
    TIFFSetField(tif, 33300, fw);
    TIFFSetField(tif, 33301, fh);

    if (!TIFFRewriteDirectory(tif)) {
        fprintf(stderr, "error: TIFFRewriteDirectory failed for '%s'\n", path);
        TIFFClose(tif);
        return 1;
    }
    TIFFClose(tif);

    /* Reopen read-only to verify the tags were persisted correctly. */
    tif = TIFFOpen(path, "r");
    if (!tif) {
        fprintf(stderr, "error: cannot reopen '%s' for verification\n", path);
        return 1;
    }
    register_pixar_tags(tif);
    print_tags(tif, path);
    TIFFClose(tif);
    return 0;
}
