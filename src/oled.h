#pragma once
#include "common.h"

// OLED burn-in guard: black wallpaper (all monitors or one) and hidden desktop icons.
// The dock already stays hidden until the pointer reaches the edge. The original
// wallpaper, background colour and icon state are kept in data\oled.ini so the mode
// survives restarts and "--restore" (uninstall, restore shortcut) can undo it.
//
// Live-wallpaper apps (Wallpaper Engine, Lively, ...) draw over the Windows wallpaper, so
// while the mode is on each of its monitors also gets a black, click-through cover window
// kept right above the desktop: under every normal window, over the live wallpaper (oled_covers.cpp).
namespace oled {
    bool Active();
    // monitor == nullptr: every monitor
    bool Enable(HMONITOR monitor);
    bool Disable();
    // after an Explorer restart or at startup: hide the desktop icons again if the mode is on
    void Reapply();
    // the desktop's own "show desktop icons" switch (also used for recordings); false on failure
    bool SetDesktopIconsVisible(bool visible);
    // every main-loop pass: shows/removes the covers to follow Active() and keeps them in z-order
    void Tick(double now);
    // monitors changed (resolution, arrangement, plugged in or out): rebuild the covers
    void InvalidateCovers();
    // the mode is on for this monitor (black wallpaper there): its dock stays hidden until called
    bool CoversMonitor(HMONITOR monitor);
}
