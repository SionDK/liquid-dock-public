// How long the main loop waits for the next look at the pointer when nothing animates
// (src/idle_wait.h): quick near the edge the docks sit at, where a dock can come up, slower elsewhere (the
// other three edges included), slower still for a pointer that rests or sits at the edge of a fullscreen
// game, never slower than an always-shown dock's glass needs - unless nobody sees the screen (locked, the display
// off). No Windows headers: builds with any C++20 compiler.
//   g++ -std=c++20 tests/idle_wait_test.cpp -o idle_wait_test
#include "../src/idle_wait.h"
#include "portable_check.h"

using namespace idlewait;

int main() {
    Check(WaitMs(true, false, false, 0) == 15, "pointer near an edge: looked at every 15 ms (as before)");
    Check(WaitMs(true, false, false, 60) == 15, "...even resting there: the dock may come up any moment");
    Check(WaitMs(false, false, false, 0.5) == 45, "pointer away from the edges, moving: every 45 ms (as before)");
    Check(WaitMs(false, false, false, kStill) == 100, "pointer resting for 2 s: every 100 ms");
    Check(WaitMs(true, true, false, 0) == 45, "at the edge of a fullscreen game's monitor (the dock does not come up there): every 45 ms, not 15");
    Check(WaitMs(true, true, false, 10) == 100, "...and resting there (an edge-scrolling game left alone): every 100 ms");
    Check(WaitMs(false, false, true, 10) == 33, "an always-shown dock's live glass still gets ~30 fps");
    Check(WaitMs(true, false, true, 10) == 15, "...and near an edge the quicker wait wins");
    // never slower than before where the dock can come up, never quicker than before anywhere
    bool ok = true;
    for (int e = 0; e < 2; ++e) for (int f = 0; f < 2; ++f) for (int g = 0; g < 2; ++g) for (double s : { 0.0, 1.9, 2.0, 30.0 }) {
        unsigned now = WaitMs(e, f, g, s);
        unsigned old = e ? 15 : 45;
        if (g) old = std::min(old, 33u);
        if (e && !f && now != old) ok = false;   // where the dock can come up: unchanged
        if (now < old) ok = false;               // nowhere quicker than before
        if (now > kStillWaitMs) ok = false;
    }
    Check(ok, "every case: unchanged where the dock can come up, nowhere quicker than before, never over 100 ms");
    Check(WaitMs(false, false, false, kStill - 0.001) == 45, "resting just under 2 s: still 45 ms");
    Check(WaitMs(false, true, false, 0) == 45 && WaitMs(false, true, false, 5) == 100, "a fullscreen app away from the edges: as anywhere away from them");
    Check(WaitMs(false, false, true, 0) == 33 && WaitMs(true, true, true, 10) == 33, "live glass caps every slower wait at 33 ms");
    Check(WaitMs(true, false, false, -1.0) == 15 && WaitMs(false, false, false, -1.0) == 45, "a move stamped after now (clock read twice): taken as moving");

    // nobody sees the screen (locked, a UAC prompt, the display off): every 100 ms, live glass or not
    Check(WaitMs(false, false, true, 10, true) == 100, "an always-shown dock's live glass on a locked screen or a display that is off: every 100 ms, not 33");
    {
        bool slowest = true, seenSame = true;
        for (int e = 0; e < 2; ++e) for (int f = 0; f < 2; ++f) for (int g = 0; g < 2; ++g) for (double s : { -1.0, 0.0, 1.9, 2.0, 30.0 }) {
            slowest &= WaitMs(e, f, g, s, true) == kStillWaitMs;
            seenSame &= WaitMs(e, f, g, s, false) == WaitMs(e, f, g, s);
        }
        Check(slowest, "...in every case: never quicker than a resting pointer away from the edge, never slower");
        Check(seenSame, "a screen someone sees: every wait as before");
        // an hour locked (or with the display off) with an always-shown dock at rest, the pointer unreadable or still
        const unsigned before = 3600 * 1000 / WaitMs(false, false, true, 3600), after = 3600 * 1000 / WaitMs(false, false, true, 3600, true);
        std::printf("  an hour nobody sees, an always-shown dock at rest: %u looks before, %u now\n", before, after);
        Check(before == 109090 && after == 36000, "...an hour of it: 109,090 looks at the pointer become 36,000");
    }

    // the display taken as off (main.cpp: the "off" notification) until a notification says on - or the pointer moves
    Check(DisplayStillOff(true, false), "the display off and the pointer still: still off");
    Check(!DisplayStillOff(true, true), "the display off and the pointer moved: someone is at the screen, taken as on");
    Check(!DisplayStillOff(false, false) && !DisplayStillOff(false, true), "a display that is on stays on, moving or not");
    // main.cpp's loop: LookAtPointer every pass, IdleWaitMs on a pass with nothing busy. An always-shown dock at rest at the
    // bottom of a desktop's 2560x1440 monitor, the pointer in the middle of it (looked at before: where it was last read).
    auto restingLook = [](bool displayOff) {
        PointerLook look; look.Start(0); look.lastX = 1280; look.lastY = 700; look.displayOff = displayOff;
        return look;
    };
    {
        // "off" comes, the pointer rests, then the user moves it and no "on" notification ever comes (a reconnected
        // session) - the glass's ~30 fps comes back at that first move
        PointerLook look = restingLook(true);
        bool slowWhileOff = true, quickAfter = true, takenOnAtTheMove = true;
        double now = 30;
        for (int frame = 0; frame < 20; ++frame) {
            const long x = frame >= 10 ? 1300 : 1280;   // moved at frame 10, resting there after
            const Look l = LookAtPointer(look, true, x, 700, now);
            takenOnAtTheMove &= l.displayTakenOn == (frame == 10) && l.moved == (frame == 10) && !l.readChanged;
            const unsigned wait = IdleWaitMs(look, l, 0, 0, 0, 2560, 1440, x, 700, false, true, now);
            if (frame < 10) slowWhileOff &= wait == kStillWaitMs;
            else quickAfter &= wait == kLiveGlassMs;
            now += wait / 1000.0;
        }
        Check(slowWhileOff && quickAfter, "...so a missed \"on\" notification leaves the loop slow only until the pointer moves");
        Check(takenOnAtTheMove && !look.displayOff, "...the display taken as on (main.cpp's 'display: taken as on' line) once, at that move");
    }
    {   // an hour of those passes, each wait as long as IdleWaitMs says (the first pass at 0 ms; tests\main_loop_test.cpp
        // counts the same on main.cpp's own loop: 36,000 locked, 109,091 seen)
        auto hour = [&](bool read, bool displayOff, int& lines) {
            PointerLook look = restingLook(displayOff);
            unsigned passes = 0;
            lines = 0;
            for (unsigned ms = 0; ms < 3600u * 1000; ++passes) {
                const double now = ms / 1000.0;
                const Look l = LookAtPointer(look, read, 1280, 700, now);
                lines += l.readChanged + l.displayTakenOn;
                const long x = read ? 1280 : kNoPointerX, y = read ? 700 : kNoPointerY;   // main.cpp: kNoPointer when unread
                ms += IdleWaitMs(look, l, 0, 0, 0, 2560, 1440, x, y, false, true, now);
            }
            return passes;
        };
        int lockedLines = 0, offLines = 0, seenLines = 0;
        const unsigned locked = hour(false, false, lockedLines), off = hour(true, true, offLines), seen = hour(true, false, seenLines);
        std::printf("  an hour, an always-shown dock at rest: %u passes locked, %u with the display off, %u seen\n", locked, off, seen);
        Check(locked == 36000 && off == 36000 && seen == 109091,
              "an hour of main.cpp's passes, an always-shown dock at rest: 36,000 locked or with the display off, 109,091 seen");
        Check(lockedLines == 1 && offLines == 0 && seenLines == 0, "...one 'pointer: unreadable' line in the locked hour, no line in the others");
    }
    {   // a pass that cannot read the pointer keeps where it was: unlocked where it was is no move, and no wait as if it moved
        PointerLook look = restingLook(false);
        const Look a = LookAtPointer(look, false, 0, 0, 10), b = LookAtPointer(look, true, 1280, 700, 11);
        Check(a.readChanged && a.unseen && !a.moved && b.readChanged && !b.unseen && !b.moved && look.lastX == 1280 && look.movedAt == 0,
              "the pointer unreadable, then read again where it was: two lines, unseen only while unread, no move");
        Check(IdleWaitMs(look, a, 0, 0, 0, 2560, 1440, kNoPointerX, kNoPointerY, false, true, 10) == kStillWaitMs &&
              IdleWaitMs(look, b, 0, 0, 0, 2560, 1440, 1280, 700, false, true, 11) == kLiveGlassMs,
              "...waits: 100 ms while unread, then as a resting pointer with live glass (33 ms)");
        const Look e = LookAtPointer(look, true, 1280, 700, 12);
        Check(!e.moved && IdleWaitMs(look, e, 0, 0, 0, 2560, 1440, 1280, 700, false, false, 12) == kStillWaitMs &&
              IdleWaitMs(look, e, 0, 0, 0, 0, 0, 1280, 700, false, false, 12) == kEdgeMs,
              "...a monitor GetMonitorInfoW could not read (all 0) is taken as it is (main.cpp): the bottom-edge rule then finds (1280, 700) near it");
    }
    {   // a move stamps movedAt (LookAtPointer): 45 ms for kStill seconds after it, then 100 ms - no live glass to cap the waits
        PointerLook look = restingLook(false);
        const Look m = LookAtPointer(look, true, 1300, 700, 30);
        Check(m.moved && look.movedAt == 30 && IdleWaitMs(look, m, 0, 0, 0, 2560, 1440, 1300, 700, false, false, 30) == kAwayMs,
              "a pointer moved away from the edge at 30 s: movedAt 30, waits 45 ms");
        const Look r1 = LookAtPointer(look, true, 1300, 700, 31.9), r2 = LookAtPointer(look, true, 1300, 700, 32.0);
        Check(!r1.moved && !r2.moved && look.movedAt == 30 &&
              IdleWaitMs(look, r1, 0, 0, 0, 2560, 1440, 1300, 700, false, false, 31.9) == kAwayMs &&
              IdleWaitMs(look, r2, 0, 0, 0, 2560, 1440, 1300, 700, false, false, 32.0) == kStillWaitMs,
              "...resting there: still 45 ms at 31.9 s, 100 ms from 32 s (kStill after the move)");
    }

    // near the docks' edge only (main.cpp): edge as Edge in common.h - 0 bottom, 1 left, 2 right, 3 top
    struct Mon { long l, t, r, b; };
    const Mon laptop = { 0, 0, 2880, 1800 }, desk = { 0, 0, 2560, 1440 }, portrait = { -1440, -1120, 0, 1440 };
    auto nearAt = [](int edge, const Mon& m, long x, long y) { return NearDockEdge(edge, m.l, m.t, m.r, m.b, x, y); };
    Check(nearAt(0, laptop, 1440, 1799) && nearAt(0, laptop, 1440, 1681) && !nearAt(0, laptop, 1440, 1680),
          "bottom dock, laptop 2880x1800: the bottom 120 px rows are near its edge");
    Check(!nearAt(0, laptop, 1440, 20) && !nearAt(0, laptop, 1440, 100) && !nearAt(0, laptop, 2870, 10) && !nearAt(0, laptop, 2870, 900),
          "...a browser's tabs, the address bar, a title bar's close button and the scroll bar are not");
    Check(nearAt(0, laptop, 0, 1799) && nearAt(0, laptop, 2879, 1799), "...both corners of its edge are");
    Check(!nearAt(0, desk, 5, 700) && !nearAt(0, portrait, -5, 0) && nearAt(0, portrait, -5, 1400),
          "bottom docks, desktop: the seam between the two monitors is not near, the portrait monitor's bottom is");
    Check(nearAt(1, portrait, -1440, 0) && !nearAt(1, portrait, -1300, 0) && nearAt(1, desk, 119, 700) && !nearAt(1, desk, 120, 700),
          "left docks: the 120 px columns at each monitor's left edge (negative coordinates too)");
    Check(nearAt(2, desk, 2559, 700) && nearAt(2, desk, 2441, 700) && !nearAt(2, desk, 2440, 700) && !nearAt(2, desk, 1280, 1439),
          "right docks: the right edge only, not the bottom");
    Check(nearAt(3, laptop, 1440, 0) && nearAt(3, laptop, 1440, 119) && !nearAt(3, laptop, 1440, 120) && !nearAt(3, portrait, -700, 1439),
          "top docks: the top edge only (there the tabs are near), not the bottom");
    {   // nowhere quicker than the old rule (any of the four edges), and the same on the docks' edge
        bool same = true;
        for (const Mon* m : { &laptop, &desk, &portrait }) for (int edge = 0; edge < 4; ++edge)
            for (long x = m->l; x < m->r; x += 37) for (long y = m->t; y < m->b; y += 41) {
                const bool old = x - m->l < 120 || m->r - x < 120 || y - m->t < 120 || m->b - y < 120;
                const long toEdge = edge == 0 ? m->b - y : edge == 1 ? x - m->l : edge == 2 ? m->r - x : y - m->t;
                if (nearAt(edge, *m, x, y) != (toEdge < 120) || (nearAt(edge, *m, x, y) && !old)) same = false;
            }
        Check(same, "every point of the three screens: near exactly within 120 px of the docks' edge, never where the old rule was not");
    }
    return Summary("idle_wait_test");
}
