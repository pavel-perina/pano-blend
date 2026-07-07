// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025 Pavel Perina

//! \file   colors.h
//! \author Pavel Perina
//! \date   July 2025
//! \brief  Color space conversions and gradient generation

#pragma once

#include <cstdint>
#include <vector>

/***************************************************************************
Useful links

Color space comparison
* https://codepen.io/web-dot-dev/pen/QWBxmxv - Gradient color spaces exploration

About OkLAB/OkLCH:
* https://bottosson.github.io/posts/oklab/ - Author's page
* https://bottosson.github.io/posts/colorpicker/ - Author's proposed HSL/HSV-like color picker
* https://evilmartians.com/chronicles/oklch-in-css-why-quit-rgb-hsl - describes advantages for web design
* https://lch.oklch.com/#0.5,24.17,92.4,100 - (Ok,Cie)LCH color picker

***************************************************************************/

namespace color {

    // ==== Structures ====================================================

    // NOTE: in range 0..1
    struct Rgb    { float r, g, b; };

    // NOTE: 8-bit sRGB (gamut-clipped, rounded to nearest)
    struct Rgb8   { uint8_t r, g, b; };

    // NOTE: L 0..100
    struct CieLab { float L, a, b; };
    struct CieLch { float L, C, H; };

    // NOTE: L 0..1
    struct OkLab  { float L, a, b; };
    struct OkLch  { float L, C, H; };

    // ==== Conversions ===================================================

    extern CieLab cieLchToLab(const CieLch& lch);
    extern CieLch cieLabToLch(const CieLab& lab);
    extern CieLab cieLabFromRgb(const Rgb& rgb);
    extern Rgb    cieLabToRgb(const CieLab& lab);

    extern OkLab  okLchToLab(const OkLch& lch);
    extern OkLch  okLabToLch(const OkLab& lab);
    extern OkLab  okLabFromRgb(const Rgb& rgb);
    extern Rgb    okLabToRgb(const OkLab& lab);

    // OkLCh → 8-bit sRGB in one step (clamp to [0,1], round to nearest).
    // Shared by the seam/label-map visualizations so their quantization agrees.
    extern Rgb8   rgb8FromOkLch(const OkLch& lch);

    // ==== Gradient Functions ============================================

    using GradientFunction = std::vector<Rgb>(*)(const Rgb&, const Rgb&, std::size_t);

    extern std::vector<Rgb> gradientRgb   (const Rgb& startRgb, const Rgb& endRgb, std::size_t length);
    extern std::vector<Rgb> gradientCieLab(const Rgb& startRgb, const Rgb& endRgb, std::size_t length);
    extern std::vector<Rgb> gradientCieLch(const Rgb& startRgb, const Rgb& endRgb, std::size_t length);
    extern std::vector<Rgb> gradientOkLab (const Rgb& startRgb, const Rgb& endRgb, std::size_t length);
    extern std::vector<Rgb> gradientOkLch (const Rgb& startRgb, const Rgb& endRgb, std::size_t length);

} // namespace color
