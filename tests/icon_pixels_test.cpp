// Tray icon pixel work (src/icon_pixels.h): glyph detection, premultiplying, shrinking big icons.
// No Windows headers: builds with any C++20 compiler.
//   cl /std:c++20 /EHsc /utf-8 tests\icon_pixels_test.cpp
//   g++ -std=c++20 tests/icon_pixels_test.cpp -o icon_pixels_test
#include "../src/icon_pixels.h"
#include "portable_check.h"

using namespace iconpx;

namespace {
uint32_t Argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return (a << 24) | (r << 16) | (g << 8) | b; }
struct Pic { int w = 0, h = 0; std::vector<uint32_t> px; };
Pic Fill(int w, int h, uint32_t v) { Pic p; p.w = w; p.h = h; p.px.assign((size_t)w * h, v); return p; }
}

int main() {
    // ---- IsGlyph
    std::vector<uint32_t> white(10, Argb(255, 255, 255, 255)), black(10, Argb(255, 0, 0, 0));
    Check(IsGlyph(white) && IsGlyph(black), "a white or a black shape is a glyph");
    Check(!IsGlyph(std::vector<uint32_t>(10, Argb(255, 128, 128, 128))), "mid grey is not (neither light nor dark)");
    Check(!IsGlyph(std::vector<uint32_t>(10, Argb(255, 255, 0, 0))), "a coloured shape is not");
    Check(!IsGlyph(std::vector<uint32_t>(7, Argb(255, 255, 255, 255))), "fewer than 8 visible pixels: not a glyph");
    std::vector<uint32_t> edged = white;
    edged.insert(edged.end(), 20, Argb(20, 255, 0, 0));   // nearly transparent red fringe: not counted
    Check(IsGlyph(edged), "pixels with alpha under 48 do not count");
    std::vector<uint32_t> near = white;
    near[3] = Argb(255, 232, 232, 232);
    Check(IsGlyph(near), "small differences (up to 24) are one colour");
    near[3] = Argb(255, 230, 230, 230);
    Check(!IsGlyph(near), "a difference over 24 is a second colour");
    Check(IsGlyph(std::vector<uint32_t>(10, Argb(255, 224, 224, 224))), "light grey (luminance about 0.88) is a light glyph");
    Check(!IsGlyph(std::vector<uint32_t>(10, Argb(255, 200, 230, 255))), "light but tinted (channels 55 apart) is not");
    Check(!IsGlyph({}), "no pixels: not a glyph");

    // ---- Premultiply
    std::vector<uint32_t> px = { Argb(255, 10, 20, 30), Argb(0, 255, 255, 255), Argb(128, 255, 255, 255), Argb(254, 1, 1, 1) };
    Premultiply(px);
    Check(px[0] == Argb(255, 10, 20, 30), "opaque pixels stay as they are");
    Check(px[1] == 0, "fully transparent pixels become 0");
    Check(px[2] == Argb(128, 128, 128, 128), "half alpha halves the colour");
    Check(px[3] == Argb(254, 0, 0, 0), "rounded down");

    // ---- Shrink
    Pic small = Fill(64, 64, Argb(255, 1, 2, 3));
    Shrink(small);
    Check(small.w == 64 && small.h == 64 && small.px.size() == 64 * 64, "64 px or smaller: left alone");
    Pic big = Fill(128, 128, Argb(255, 9, 8, 7));
    Shrink(big);
    Check(big.w == 64 && big.h == 64 && big.px.size() == 64 * 64 && big.px[0] == Argb(255, 9, 8, 7) && big.px.back() == Argb(255, 9, 8, 7),
          "128 px: halved, a plain colour stays that colour");
    Pic wide = Fill(100, 50, 0);
    Shrink(wide);
    Check(wide.w == 50 && wide.h == 25, "100 x 50: factor 2 for both sides (the longer side decides)");
    Pic two; two.w = 130; two.h = 1;
    for (int x = 0; x < 130; ++x) two.px.push_back(x % 2 ? Argb(255, 255, 255, 255) : Argb(0, 0, 0, 0));
    Shrink(two);
    Check(two.w == 44 && two.h == 1, "130 x 1: factor 3 (ceil 130/64) gives 44 x 1");
    Check(two.px[0] == Argb(85, 85, 85, 85), "a block of three pixels is averaged, rounded down (255/3 = 85)");
    Check(two.px[43] == Argb(255, 255, 255, 255), "the last partial block (only pixel 129, white) averages only what it has");
    Pic tiny = Fill(32, 32, Argb(255, 50, 50, 50));
    Shrink(tiny, 16);
    Check(tiny.w == 16 && tiny.h == 16, "another limit: 32 px to 16");
    Pic guard = Fill(80, 80, 0);
    Shrink(guard, 0);
    Check(guard.w == 80, "a limit of 0 changes nothing");

    // ---- edges: the fewest pixels, the reference colour, Direct2D's rule, partial blocks, the largest picture
    Check(IsGlyph(std::vector<uint32_t>(8, Argb(255, 255, 255, 255))), "exactly 8 visible pixels: a glyph");
    {
        std::vector<uint32_t> v(1, Argb(47, 255, 0, 0));   // a faint red pixel first: not counted, not the reference
        v.insert(v.end(), 9, Argb(255, 0, 0, 0));
        Check(IsGlyph(v), "a faint pixel before the shape does not become the reference colour");
    }
    {
        std::vector<uint32_t> v;
        for (uint32_t a = 0; a < 256; a += 5) v.push_back(Argb(a, 255, 200, 1));
        Premultiply(v);
        bool ok = true;
        for (uint32_t p : v) {
            uint32_t a = p >> 24;
            if (((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a) ok = false;
        }
        Check(ok, "premultiplied: no colour channel above alpha (what Direct2D requires)");
    }
    {
        Pic p = Fill(65, 65, Argb(255, 0, 0, 0));
        p.px.back() = Argb(255, 200, 100, 50);
        Shrink(p);
        Check(p.w == 33 && p.h == 33 && p.px.back() == Argb(255, 200, 100, 50) && p.px[0] == Argb(255, 0, 0, 0),
              "65 x 65: factor 2 to 33 x 33, the bottom right block is its one pixel");
    }
    {
        Pic p = Fill(10, 200, 0);
        Shrink(p);
        Check(p.w == 3 && p.h == 50 && p.px.size() == 150, "10 x 200 (tall): factor 4, 3 x 50");
    }
    {
        Pic p = Fill(1024, 1024, Argb(255, 255, 255, 255));   // the largest picture the tray host reads
        Shrink(p);
        Check(p.w == 64 && p.h == 64 && p.px[0] == Argb(255, 255, 255, 255) && p.px.back() == Argb(255, 255, 255, 255),
              "1024 px: factor 16 to 64, the sums of 256 white pixels do not overflow");
    }
    {
        Pic p; p.w = 128; p.h = 1;
        for (int x = 0; x < 128; ++x) p.px.push_back(x % 2 ? Argb(255, 255, 0, 0) : 0);
        Shrink(p);
        Check(p.w == 64 && p.px[0] == Argb(127, 127, 0, 0), "transparent next to opaque red: half alpha, half colour (premultiplied)");
    }
    return Summary("icon_pixels_test");
}
