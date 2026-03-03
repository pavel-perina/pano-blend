// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025 Pavel Perina

// Portions of this file are derived from https://bottosson.github.io/posts/oklab/
// Copyright (c) 2020 Björn Ottosson
// Licensed under the MIT License

#include "colors.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr float TO_RADIANS = 0.017453292519943295f;
constexpr float TO_DEGREES = 57.29577951308232f;


// Gamma decode sRGB → Linear RGB
inline float invGamma(float c)
{
    return (c <= 0.04045f)
        ? (c / 12.92f)
        : std::pow((c + 0.055f) / 1.055f, 2.4f);
}


// Gamma encode Linear RGB → sRGB
inline float gammaCorrect(float c)
{
    return (c <= 0.0031308f)
        ? (12.92f * c)
        : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
}


inline float gammaCorrectAndLimit(float c)
{
    c = gammaCorrect(c);
    c = std::clamp(c, 0.0f, 1.0f);
    return c;
}


inline color::Rgb gammaCorrectAndLimit(const color::Rgb &color)
{
    return{ gammaCorrectAndLimit(color.r),gammaCorrectAndLimit(color.g),gammaCorrectAndLimit(color.b) };
}


std::array<float, 3> lchDeltas(const float* lchStart, const float* lchEnd, size_t steps)
{
    const float deltaL = lchEnd[0] - lchStart[0];
    const float deltaC = lchEnd[1] - lchStart[1];
    float       deltaH = lchEnd[2] - lchStart[2];
    if (deltaH > 180.0f)
        deltaH -= 360.0f;
    if (deltaH < -180.0f)
        deltaH += 360.0f;
    std::array result{ deltaL, deltaC, deltaH };
    if (steps > 2) {
        float k = 1.0f / static_cast<float>(steps);
        for (int i = 0; i < (int)result.size(); ++i) {
            result[i] *= k;
        }
    }
    return result;
}


template<typename LabT, typename LchT>
LabT convLchToLab(const LchT& lch)
{
    const float h_rad = lch.H * TO_RADIANS;
    return { lch.L, lch.C * std::cos(h_rad), lch.C * std::sin(h_rad) };
}


template<typename LchT, typename LabT>
LchT convLabToLch(const LabT& lab)
{
    float C = std::sqrt(lab.a * lab.a + lab.b * lab.b);
    float H = std::atan2(lab.b, lab.a) * TO_DEGREES;
    if (H < 0.0f)
        H += 360.0f;
    return { lab.L, C, H };
}


} // anonymous namespace

namespace color {

CieLab cieLchToLab(const CieLch& lch)
{
    return convLchToLab<CieLab, CieLch>(lch);
}


CieLch cieLabToLch(const CieLab& lab)
{
    return convLabToLch<CieLch, CieLab>(lab);
}


OkLab okLchToLab(const OkLch& lch)
{
    return convLchToLab<OkLab, OkLch>(lch);
}


OkLch okLabToLch(const OkLab& lab)
{
    return convLabToLch<OkLch, OkLab>(lab);
}


CieLab cieLabFromRgb(const Rgb& rgb)
{
    const float r = invGamma(rgb.r);
    const float g = invGamma(rgb.g);
    const float b = invGamma(rgb.b);

    // Convert to XYZ (sRGB D65)
    float x = r * 0.4124f + g * 0.3576f + b * 0.1805f;
    float y = r * 0.2126f + g * 0.7152f + b * 0.0722f;
    float z = r * 0.0193f + g * 0.1192f + b * 0.9505f;

    // Normalize by D65 white point
    x /= 0.95047f;
    y /= 1.00000f;
    z /= 1.08883f;

    // f(t) helper
    auto f = [](float t) -> float {
        return (t > 0.008856f)
            ? std::cbrt(t)
            : (7.787f * t + 16.0f / 116.0f);
    };

    const float fx = f(x);
    const float fy = f(y);
    const float fz = f(z);

    return {
        (116.0f * fy) - 16.0f,
        500.0f * (fx - fy),
        200.0f * (fy - fz)
    };
}


Rgb cieLabToRgb(const CieLab& lab)
{
    // D65 white point
    constexpr float refX = 95.047f;
    constexpr float refY = 100.000f;
    constexpr float refZ = 108.883f;

    float y = (lab.L + 16.0f) / 116.0f;
    float x = lab.a / 500.0f + y;
    float z = y - lab.b / 200.0f;

    x = (x * x * x > 0.008856f) ? (x * x * x) : ((x - 16.0f / 116.0f) / 7.787f);
    y = (y * y * y > 0.008856f) ? (y * y * y) : ((y - 16.0f / 116.0f) / 7.787f);
    z = (z * z * z > 0.008856f) ? (z * z * z) : ((z - 16.0f / 116.0f) / 7.787f);

    x *= refX;
    y *= refY;
    z *= refZ;

    // XYZ to linear RGB
    const Rgb rgb {
        x *  0.032406f + y * -0.015372f + z * -0.004986f,
        x * -0.009689f + y *  0.018758f + z *  0.000415f,
        x *  0.000557f + y * -0.002040f + z *  0.010570f
    };

    return gammaCorrectAndLimit(rgb);
}


OkLab okLabFromRgb(const Rgb& rgb)
{
    // 1. sRGB → linear
    const float r = invGamma(rgb.r);
    const float g = invGamma(rgb.g);
    const float b = invGamma(rgb.b);

    // 2. Linear sRGB → LMS
    const float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    const float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    const float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

    // 3. Nonlinear transform (cube root)
    const float l_ = cbrtf(l);
    const float m_ = cbrtf(m);
    const float s_ = cbrtf(s);

    // 4. LMS → OKLab
    return {
        +0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
        +1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
        +0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_
    };
}


Rgb okLabToRgb(const OkLab& lab)
{
    // 1. OKLab → LMS nonlinear
    const float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    const float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    const float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;

    // 2. Remove nonlinearity (cube)
    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;

    // 3. LMS → linear sRGB
    const Rgb rgb{
        +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s
    };

    // 4. Linear → sRGB (gamma encode)
    return gammaCorrectAndLimit(rgb);
}


std::vector<Rgb> gradientCieLch(const Rgb& startRgb, const Rgb& endRgb, std::size_t length)
{
    if (length == 0)
        return {};
    if (length == 1)
        return { startRgb };

    std::vector<Rgb> gradient(length);

    const CieLch startLch = cieLabToLch(cieLabFromRgb(startRgb));
    const CieLch endLch   = cieLabToLch(cieLabFromRgb(endRgb));
    if (startLch.C < 0.015 || endLch.C < 0.015) { // includes white
        return gradientCieLab(startRgb, endRgb, length);
    }

    auto deltas = lchDeltas(&startLch.L, &endLch.L, length-1);

    for (size_t i = 0; i < length; ++i) {
        CieLch lch {
            startLch.L + i * deltas[0],
            startLch.C + i * deltas[1],
            startLch.H + i * deltas[2],
        };
        gradient[i] = cieLabToRgb(cieLchToLab(lch));
    }

    return gradient;
}


std::vector<Rgb> gradientOkLch(const Rgb& startRgb, const Rgb& endRgb, std::size_t length)
{
    if (length == 0)
        return {};
    if (length == 1)
        return { startRgb };

    std::vector<Rgb> gradient(length);

    const OkLch startLch = convLabToLch<OkLch, OkLab>(okLabFromRgb(startRgb));
    const OkLch endLch   = convLabToLch<OkLch, OkLab>(okLabFromRgb(endRgb));
    if (startLch.C < 1.0e-3 || endLch.C < 1.0e-3) {
        return gradientOkLab(startRgb, endRgb, length);
    }

    auto deltas = lchDeltas(&startLch.L, &endLch.L, length - 1);

    for (size_t i = 0; i < length; ++i) {
        OkLch lch{
            startLch.L + i * deltas[0],
            startLch.C + i * deltas[1],
            startLch.H + i * deltas[2],
        };
        gradient[i] = okLabToRgb(convLchToLab<OkLab, OkLch>(lch));
    }

    return gradient;
}


std::vector<Rgb> gradientCieLab(const Rgb& startRgb, const Rgb& endRgb, std::size_t length)
{
    if (length == 1)
        return { startRgb };

    std::vector<Rgb> gradient(length);

    CieLab startLab = cieLabFromRgb(startRgb);
    CieLab endLab   = cieLabFromRgb(endRgb);

    for (size_t i = 0; i < length; ++i) {
        float t = (float)i / (length - 1);

        CieLab lab {
            (1.0f - t) * startLab.L + t * endLab.L,
            (1.0f - t) * startLab.a + t * endLab.a,
            (1.0f - t) * startLab.b + t * endLab.b
        };

        gradient[i] = cieLabToRgb(lab);
    }

    return gradient;
}


std::vector<Rgb> gradientOkLab(const Rgb& startRgb, const Rgb& endRgb, std::size_t length)
{
    if (length == 1)
        return { startRgb };

    std::vector<Rgb> gradient(length);

    OkLab startLab = okLabFromRgb(startRgb);
    OkLab endLab   = okLabFromRgb(endRgb);

    for (size_t i = 0; i < length; ++i) {
        float t = (float)i / (length - 1);

        OkLab lab{
            (1.0f - t) * startLab.L + t * endLab.L,
            (1.0f - t) * startLab.a + t * endLab.a,
            (1.0f - t) * startLab.b + t * endLab.b
        };

        gradient[i] = okLabToRgb(lab);
    }

    return gradient;
}


std::vector<Rgb> gradientRgb(const Rgb& startRgb, const Rgb& endRgb, std::size_t length)
{
    std::vector<Rgb> gradient(length);
    if (length == 1)
        return { startRgb };

    for (size_t i = 0; i < length; ++i) {
        float t = (float)i / (length - 1);
        gradient[i].r = (1.0f - t) * startRgb.r + t * endRgb.r;
        gradient[i].g = (1.0f - t) * startRgb.g + t * endRgb.g;
        gradient[i].b = (1.0f - t) * startRgb.b + t * endRgb.b;
    }

    return gradient;
}

} // namespace color
