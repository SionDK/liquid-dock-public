# Task T4: edge-case tests for src/hwinfo.h and src/hwinfo.cpp

Write `tests/llm/hwinfo_more_test.cpp`, a test program for the parts of `src/hwinfo.h` / `src/hwinfo.cpp` (below) that need no registry: `ParseTemperature`, `RankCpu`, `PickCpu` and `Judge`. They read the CPU temperature HWiNFO reports.

Requirements:
- Include exactly `#include "src/hwinfo.h"` and `#include "tests/portable_check.h"` (the build adds the repository root to the include path and compiles src/hwinfo.cpp with your file), plus any standard headers you need. Do not use `hwinfo::Reader` (it reads the registry).
- `int main()` calls `Check(condition, "what this check pins down")` for every check and ends with `return Summary("hwinfo_more_test");`.
- Use wide strings (`L"..."`). Write the degree sign as `L"°"` and the single-character signs as `L"℃"` (degree Celsius) and `L"℉"` (degree Fahrenheit) so the file stays ASCII.
- Compare temperatures with a tolerance, for example `std::fabs(c - expected) < 0.05f`.
- At least 25 checks, each a distinct case. Group them by function with a short comment.
- Add cases the existing test (below) does not have, for example:
  - `ParseTemperature`: the limits (more than -40 and less than 150 are accepted: test -40, -39.9, 149.9, 150, and the same limits reached through Fahrenheit, e.g. 302 F = 150 C); Fahrenheit with the degree sign and with U+2109; U+2103; lower-case `c` / `f` after the degree sign; a degree sign followed by something else; a raw value that is empty or not a number (the formatted value is used then); a decimal comma; a `+` sign; a non-breaking space (U+00A0) between number and unit; that `celsius` is left unchanged when the result is false.
  - `RankCpu`: every rank 0 to 7 and -1; case-insensitive matching; a preference that matches; an empty preference with an empty label; "Core Max" in a sensor that does not start with "CPU [" (-1); a label containing "cpu" in a CPU sensor (6) and outside one.
  - `PickCpu`: two readings of the same rank (the earlier one stays); a well-ranked reading whose value is not a temperature (skipped); the returned index is a position in the vector, not the `index` field; `celsius` unchanged when nothing is found.
  - `Judge`: the 20 s and 60 s limits exactly (20.0, 19.9, 60.0, 60.1), with HWiNFO running and not running; that the process check is asked only when the key is 20 s or older; no values at all.

Your test is also run against copies of the code with one deliberate bug each (for example a limit moved, or a conversion turned around). A good test fails on most of them, so test both sides of every limit.
