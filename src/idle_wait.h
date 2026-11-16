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

}   // namespace idlewait
