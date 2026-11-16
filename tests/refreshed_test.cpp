// A value read again at most every `period` seconds (src/refreshed.h), as the audio button's tooltip
// uses it: the first look reads, looks within the period do not, the first look after it does, a
// change made by the dock itself (Invalidate) is read at the next look, and a clock that goes back
// does not freeze the value. Over a simulated minute of hovering at 144 frames a second the device
// name is read 60 times instead of 8640. No Windows headers: builds with any C++20 compiler.
//   g++ -std=c++20 tests/refreshed_test.cpp -o refreshed_test
#include "../src/refreshed.h"
#include "portable_check.h"
#include <cstdio>
#include <string>

int main() {
    int reads = 0;
    std::string name = "Speakers";
    auto read = [&] { ++reads; return name; };
    Refreshed<std::string> v;

    Check(v.Get(10.0, 1.0, read) == "Speakers" && reads == 1, "the first look reads");
    Check(v.Get(10.5, 1.0, read) == "Speakers" && reads == 1, "a look within the period does not");
    name = "Headphones";
    Check(v.Get(10.99, 1.0, read) == "Speakers" && reads == 1, "...even when the value has changed meanwhile (seen at the next read)");
    Check(v.Get(11.0, 1.0, read) == "Headphones" && reads == 2, "the first look after the period reads again");
    Check(v.Get(11.2, 1.0, read) == "Headphones" && reads == 2, "...and starts a new period");
    name = "Monitor";
    v.Invalidate();
    Check(v.Get(11.3, 1.0, read) == "Monitor" && reads == 3, "after Invalidate (the dock switched the device itself) the next look reads");
    Check(v.Get(11.4, 1.0, read) == "Monitor" && reads == 3, "...once");
    Check(v.Get(5.0, 1.0, read) == "Monitor" && reads == 4, "a clock that went back reads again (does not wait for it to catch up)");
    Check(v.Get(5.5, 1.0, read) == "Monitor" && reads == 4, "...and counts from there");
    Check(v.Get(1e6, 1.0, read) == "Monitor" && reads == 5, "a look after a long time reads (the pointer came back hours later)");

    {   // a minute of hovering the button at 144 frames a second
        Refreshed<std::string> tip;
        int n = 0;
        auto count = [&] { ++n; return std::string("x"); };
        for (int f = 0; f < 60 * 144; ++f) tip.Get(100.0 + f / 144.0, 1.0, count);
        std::printf("a minute of hovering at 144 fps: %d reads (every frame: %d)\n", n, 60 * 144);
        Check(n >= 59 && n <= 61, "a minute of hovering reads the device about once a second, not every frame");
    }
    {   // period 0: every look reads (a value that must always be fresh)
        Refreshed<int> z;
        int n = 0;
        for (int i = 0; i < 5; ++i) z.Get(1.0, 0.0, [&] { return ++n; });
        Check(n == 5, "period 0 reads at every look");
    }
    {   // Invalidate before the first look, a long uptime, a value of another type
        Refreshed<int> u;
        int n = 0;
        u.Invalidate();
        Check(u.Get(0.0, 1.0, [&] { return ++n; }) == 1 && n == 1, "Invalidate before any look: the first look reads once");
        Check(u.Get(0.0, 1.0, [&] { return ++n; }) == 1 && n == 1, "...a look at the same moment does not (a clock that reads 0 is a time like any)");
        const double year = 365.0 * 24 * 3600;   // QueryPerformanceCounter seconds after a year of uptime
        Check(u.Get(year, 1.0, [&] { return ++n; }) == 2, "a year of uptime: read");
        Check(u.Get(year + 0.999, 1.0, [&] { return ++n; }) == 2 && u.Get(year + 1.0, 1.0, [&] { return ++n; }) == 3,
              "...and the period still holds to the millisecond there");
        Refreshed<std::wstring> w;
        Check(w.Get(1.0, 1.0, [] { return std::wstring(L"스피커"); }) == L"스피커", "a wide string value (the device name) is kept as read");
    }
    return Summary("refreshed_test");
}
