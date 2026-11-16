# Task T3: tests for src/icon_pixels.h

Write `tests/llm/icon_pixels_more_test.cpp`, a test program for `src/icon_pixels.h` (below): pixel work on tray icon pictures. Pixels are `uint32_t` values 0xAARRGGBB (alpha in the top byte, then red, green, blue).

Requirements:
- Include exactly `#include "src/icon_pixels.h"` and `#include "tests/portable_check.h"` (the build adds the repository root to the include path), plus any standard headers you need.
- `int main()` calls `Check(condition, "what this check pins down")` for every check and ends with `return Summary("icon_pixels_more_test");`.
- A helper like `uint32_t Argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return (a << 24) | (r << 16) | (g << 8) | b; }` and a picture struct `struct Pic { int w = 0, h = 0; std::vector<uint32_t> px; };` make the tests short (`Shrink` takes any struct with `int w, h` and `std::vector<uint32_t> px`).
- At least 20 checks, each a distinct case. Group them by function with a short comment.
- Test the exact limits in the comments and the code:
  - `IsGlyph`: alpha 47 does not count and alpha 48 does; exactly 7 and exactly 8 counted pixels; a channel difference of exactly 24 (still one colour) and 25 (a second colour); a light colour with channels 23 apart and one 24 or more apart; light (luminance over 0.82) and dark (under 0.18) glyphs, white and black.
  - `Premultiply`: alpha 255, 254, 128, 1 and 0; that results are rounded down (for example red 1 at alpha 128 gives 0, red 255 at alpha 128 gives 128).
  - `Shrink`: a picture exactly `maxSide` px, `maxSide + 1` px, a wide and a tall picture, factors 2 and 3, the averaging of a block with different pixels, a partial block at the right or bottom edge (averaged over the pixels it has), a different `maxSide`, `maxSide` 0 or negative (nothing changes).
- Compute expected values by hand from the formulas in the code, and say in the check text what is expected.

Your test is also run against copies of the header with one deliberate bug each (for example a limit moved by one). A good test fails on most of them, so test both sides of every limit.
