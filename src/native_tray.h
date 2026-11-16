#pragma once
#include "common.h"

// Opens Windows' own tray windows from the dock instead of re-implementing them.
namespace nativetray {
    // Why the hidden-icons flyout did not open: NoButton = Windows shows no hidden-icons button (nothing
    // hidden, or its "hidden icon menu" turned off); NotOpened = pressed, no flyout came (or no taskbar).
    enum class OverflowFailure { None, NoButton, NotOpened };
    // The native hidden-icons flyout, anchored next to the dock on Windows 10/11.
    // Fails without exposing the taskbar if the visibility guard cannot be acquired.
    bool OpenOverflow(POINT anchor, Edge edge, RECT dockBar, OverflowFailure* why = nullptr);
    // Stock Windows 11 taskbar: remember its tray buttons (call before hiding it; UI thread). The network and clock
    // buttons are found from the taskbar's window or, the taskbar hidden already, from its XAML island; a click looks
    // them up again when none is kept. The hidden-icons button only while the taskbar is expanded (OpenOverflow
    // expands it for that).
    void CacheTrayButtons(HWND tray);
    // The notification centre with the calendar: the taskbar's own clock button pressed (a second click closes it, as
    // on the taskbar), else Win+N; Win+Alt+D on a classic taskbar. dock.log: "tray: calendar". UI thread.
    void OpenCalendar();
    void OpenVolume(POINT center);          // volume slider at a screen position
    // Wi-Fi, Bluetooth, ...: the taskbar's network button likewise, else Win+A. dock.log: "tray: quick settings".
    void OpenQuickSettings();
    bool ToggleDesktop();                  // Explorer owns show/restore state
}
