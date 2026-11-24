#pragma once
// How long the dock's main loop may wait for the next look at the pointer while nothing animates
// (main.cpp), kept free of Windows headers so it can be tested anywhere (tests/idle_wait_test.cpp, run by
// tests/run_portable.sh). Windows tells the dock nothing about the pointer without a hook, so the loop
// looks: often near the screen edge the docks sit at, where a dock comes up; less often elsewhere.
//
// Cases looked at as often as near that edge with nothing to gain: a pointer at the edge of a monitor
// that a fullscreen app (a game) fills, where the dock does not come up; a pointer that has not moved
// for a while (reading, a video, a game that keeps it in the middle); and a pointer near one of the
// other three edges (a browser's tabs, a title bar's buttons, a scroll bar, the seam between two
// monitors), where no dock comes up either. There the loop now waits longer. Messages - a hotkey, a
// window activated (Start opening), a notification - end the wait at once whatever its length, so only
// the pointer is looked at less often; the first move is seen within kStillWaitMs, and near the docks'
// edge the loop is quick again.
#include <algorithm>
#include <climits>

namespace idlewait {

constexpr unsigned kEdgeMs = 15;       // pointer near the edge where the dock can come up
constexpr unsigned kAwayMs = 45;       // elsewhere
constexpr unsigned kStillWaitMs = 100; // the pointer has not moved for kStill seconds (and not at a live edge)
constexpr unsigned kLiveGlassMs = 33;  // an always-shown dock's glass follows the screen at ~30 fps
constexpr double kStill = 2.0;
constexpr long kEdgeZone = 120;        // px from the docks' edge that count as near it (the capture pre-warm
                                       // reaches 60 px times the dock's scale: 75 px at a 1.25 scale)

// The pointer (x, y) near the edge the docks sit at - edge as Edge in common.h: 0 bottom, 1 left, 2 right,
// 3 top - of its monitor (left, top, right, bottom: rcMonitor). Every dock sits at that one edge of its
// monitor, and only there does the pointer bring one up (Dock::Update).
inline bool NearDockEdge(int edge, long left, long top, long right, long bottom, long x, long y) {
    switch (edge) {
    case 0: return bottom - y < kEdgeZone;
    case 1: return x - left < kEdgeZone;
    case 2: return right - x < kEdgeZone;
    default: return y - top < kEdgeZone;
    }
}

// unseen: nobody can see the screen - another desktop has the input (the lock screen, a UAC prompt: the pointer
// cannot be read) or the session's display is off. No dock comes up and no glass is seen then, so the loop looks as
// at a resting pointer away from the edge, live glass or not (~30 looks a second become 10). Unlocking is seen within
// kStillWaitMs; the display coming on is a message, seen at once.
inline unsigned WaitMs(bool nearEdge, bool fullscreenHere, bool liveGlass, double stillFor, bool unseen = false) {
    if (unseen) return kStillWaitMs;
    unsigned wait;
    if (nearEdge && !fullscreenHere) wait = kEdgeMs;
    else wait = stillFor >= kStill ? kStillWaitMs : kAwayMs;
    if (liveGlass) wait = (std::min)(wait, kLiveGlassMs);   // (std::min): safe from windows.h's macro
    return wait;
}

// Whether the display is still taken as off (main.cpp: set by the GUID_SESSION_DISPLAY_STATUS notification) after
// a look at the pointer. A pointer that moved means someone is at the screen - input wakes the display - so the
// loop goes back to its usual waits even if no "on" notification ever comes (a session reconnected, a remote one).
// A move that was nobody's (a program setting the pointer, monitors rearranged) only brings back the waits of a
// display that is on.
inline bool DisplayStillOff(bool displayOff, bool pointerMoved) { return displayOff && !pointerMoved; }

// The point the docks see while the pointer cannot be read (main.cpp's kNoPointer): off every monitor, so they go
// away as when it leaves.
constexpr long kNoPointerX = -32000, kNoPointerY = -32000;

// What main.cpp's loop keeps from one look at the pointer to the next (one object there, g_look, that MainProc
// shares: it sets displayOff from the GUID_SESSION_DISPLAY_STATUS notification, also before the loop starts).
struct PointerLook {
    long lastX = LONG_MIN, lastY = LONG_MIN;   // where it was last read: a pass that cannot read it keeps this, so a
                                               // pointer unlocked where it was is no move
    double movedAt = 0;        // when it last moved
    bool lastRead = true;      // whether the pass before could read it
    bool displayOff = false;   // the session's display off: until an "on" notification or a move
    // The loop starts (now: its first clock reading): no pointer read yet, displayOff as the notifications left it
    void Start(double now) { lastX = lastY = LONG_MIN; movedAt = now; lastRead = true; }
};

// One pass's look at the pointer (main.cpp: every pass, busy or not - GetCursorPos, then this, then the lines it asks for).
struct Look {
    bool read;             // GetCursorPos worked
    bool readChanged;      // ...and the pass before it did not, or the other way round: a "pointer:" line
    bool moved;            // read, and not where it was last read
    bool displayTakenOn;   // the display was taken as off and the pointer moved: the "display: taken as on" line
    bool unseen;           // nobody sees the screen: the pointer unreadable (locked, a UAC prompt) or the display off
};
inline Look LookAtPointer(PointerLook& s, bool read, long x, long y, double now) {
    Look l{};
    l.read = read;
    if (read != s.lastRead) { s.lastRead = read; l.readChanged = true; }
    l.moved = read && (x != s.lastX || y != s.lastY);
    if (l.moved) { s.lastX = x; s.lastY = y; s.movedAt = now; }
    if (s.displayOff && !DisplayStillOff(s.displayOff, l.moved)) { s.displayOff = false; l.displayTakenOn = true; }
    l.unseen = !read || s.displayOff;
    return l;
}

// The wait of a pass with nothing busy (main.cpp: only then does it ask for the pointer's monitor, (left, top, right,
// bottom) its rcMonitor - all 0 if GetMonitorInfoW failed). (x, y): the point the docks saw, kNoPointer when unread;
// fullscreenHere: a dock on that monitor has a fullscreen app over it.
inline unsigned IdleWaitMs(const PointerLook& s, const Look& look, int edge, long left, long top, long right, long bottom,
                           long x, long y, bool fullscreenHere, bool liveGlass, double now) {
    const bool nearEdge = look.read && NearDockEdge(edge, left, top, right, bottom, x, y);
    return WaitMs(nearEdge, fullscreenHere, liveGlass, now - s.movedAt, look.unseen);
}

}   // namespace idlewait
