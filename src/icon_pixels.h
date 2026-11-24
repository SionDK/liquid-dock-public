#pragma once
// Pixel work on tray icon pictures (tray_host_icons.cpp) and app icons (apps.cpp), free of Windows headers so it
// can be tested with any C++20 compiler (tests/icon_pixels_test.cpp). Pixels are 32-bit BGRA as GetDIBits gives
// them: alpha in the top byte, then red, green, blue (0xAARRGGBB), rows top-down.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace iconpx {

// One white or black colour (taskbar glyphs drawn for the dark or light theme): the dock tints it with
// its own ink colour. Pixels with alpha under 48 do not count; at least 8 pixels must; all counted
// pixels within 24 levels of the first one on each channel; that colour grey (channels within 24 of
// each other) and either light (luminance over 0.82) or dark (under 0.18).
inline bool IsGlyph(const std::vector<uint32_t>& px) {
    int count = 0, r0 = -1, g0 = 0, b0 = 0;
    for (uint32_t p : px) {
        if ((p >> 24) < 48) continue;
        int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        if (r0 < 0) { r0 = r; g0 = g; b0 = b; }
        else if (std::abs(r - r0) > 24 || std::abs(g - g0) > 24 || std::abs(b - b0) > 24) return false;
        ++count;
    }
    if (count < 8) return false;
    float lum = (0.2126f * r0 + 0.7152f * g0 + 0.0722f * b0) / 255.0f;
    return std::max({ r0, g0, b0 }) - std::min({ r0, g0, b0 }) < 24 && (lum > 0.82f || lum < 0.18f);
}

// Straight alpha to premultiplied alpha (what Direct2D draws): each colour channel times alpha / 255,
// rounded down. Fully opaque pixels stay as they are.
inline void Premultiply(std::vector<uint32_t>& px) {
    for (auto& p : px) {
        uint32_t a = p >> 24;
        if (a == 255) continue;
        uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
        p = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

// Whether any pixel has some alpha: a picture with none (an old 32-bit bitmap, an icon of colour and mask) leaves
// its alpha byte empty rather than meaning it invisible.
inline bool AnyAlpha(const std::vector<uint32_t>& px) {
    for (uint32_t p : px) if (p >> 24) return true;
    return false;
}

// Every pixel fully opaque, its colour as it was.
inline void MakeOpaque(std::vector<uint32_t>& px) {
    for (auto& p : px) p |= 0xff000000;
}

// Tray icons are drawn at 16-24 px: a program that hands over a big icon is kept at `maxSide` px at
// most. Shrinks by a whole factor f = ceil(longer side / maxSide) with a box filter (the average of each
// f x f block, partial blocks at the right and bottom edges averaged over the pixels they have), every
// channel rounded down. Works on premultiplied pixels. Picture: any struct with int w, h and a
// std::vector<uint32_t> px of w * h pixels.
template <class Picture>
void Shrink(Picture& p, int maxSide = 64) {
    if (maxSide <= 0 || (p.w <= maxSide && p.h <= maxSide)) return;
    int f = (std::max(p.w, p.h) + maxSide - 1) / maxSide;
    int nw = (p.w + f - 1) / f, nh = (p.h + f - 1) / f;
    std::vector<uint32_t> out((size_t)nw * nh);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            uint32_t sum[4] = {}, n = 0;
            for (int yy = y * f; yy < std::min(p.h, (y + 1) * f); ++yy)
                for (int xx = x * f; xx < std::min(p.w, (x + 1) * f); ++xx, ++n) {
                    uint32_t px = p.px[(size_t)yy * p.w + xx];
                    for (int c = 0; c < 4; ++c) sum[c] += (px >> (c * 8)) & 255;
                }
            uint32_t v = 0;
            for (int c = 0; c < 4; ++c) v |= (n ? sum[c] / n : 0) << (c * 8);
            out[(size_t)y * nw + x] = v;
        }
    p.w = nw; p.h = nh; p.px = std::move(out);
}

}   // namespace iconpx
