#pragma once
// What the dock shares with the settings window, so that settings_window*.cpp stand without dock.h (the Dock class):
// the tray's proportions and the settings window's hold on the docks. dock.h includes it. Declarations, inline
// arithmetic and one plain global only: no Windows calls here (a test's fakes may come after this header).
#include "common.h"

// The tray's proportions from the settings, one rule for the dock (Dock::MeasureMetrics) and the settings window's
// preview (settingswindow::DrawCustomizer). Each caller keeps its own rounding of the gap, and the thickness it pairs
// by: the dock's after its padding, the preview's before. The space between icons, as a part of the icon size:
inline float IconGapRatio() {
    constexpr float spacing[] = { 0.22f, 0.32f, 0.44f };
    return g_settings.iconGap > 0 ? std::clamp(g_settings.iconGap, 10, 60) / 100.0f : spacing[std::clamp(g_settings.iconSpacing, 0, 2)];
}
// the glass thickness setting, as a part of the default thickness (icons are 60% of that)
inline float ThicknessRatio() { return std::clamp(g_settings.dockThickness, 75, 150) / 100.0f; }
// pair the small buttons while half the bar still makes a comfortable target
inline bool StackButtons(float thickness, float scale) { return thickness * 0.5f >= 22 * scale; }

// The shape settings window keeps every dock shown while that window can be seen (SettingsHold in dock.cpp). The
// settings window writes it (settings_window.cpp), the docks read it. Two parts, apart on purpose:
//   open  set (with wnd) only once the window's device resources are made (Show); false at once on a minimize while
//         the window and its handle still live (WM_SIZE), and on WM_DESTROY. Dock::Create snaps its reveal by it alone.
//   wnd   the window that holds the docks up (cleared on WM_DESTROY). Open without a handle holds them up all the same.
struct SettingsHoldState {
    bool open;
    HWND wnd;
};
inline SettingsHoldState g_settingsHold{};   // zero: closed, no window (constant-initialized, no constructor runs)
