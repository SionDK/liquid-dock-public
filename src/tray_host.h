#pragma once
#include "common.h"

// Tray icons on the dock (experimental; Settings::trayIcons).
//
// Windows 11 draws its tray icons inside Explorer's XAML taskbar, where other programs cannot read
// them. Programs add and change their icons with Shell_NotifyIcon, which sends WM_COPYDATA to the
// first top-level "Shell_TrayWnd" window. The tray host owns such a window (hidden, kept above
// Explorer's, as RetroBar / ManagedShell do), so every icon reaches it first. Unlike a replacement
// shell it hands every message on to Explorer unchanged: Explorer keeps all icons, its hidden-icons
// flyout and its notifications, and loses nothing if the dock closes or crashes.
//
// The dock shows the icons Windows shows on the taskbar (Settings > Personalization > Taskbar >
// Other system tray icons: HKCU\Control Panel\NotifyIconSettings\*\IsPromoted), in Windows' order.
// Its .cpp files and what each holds: tray_host_internal.h.
namespace trayhost {
    // UI thread. True when the host runs (also when it already did). announce: ask every program to
    // re-add its icons (TaskbarCreated) so the dock gets them; without, the host only passes them on.
    bool Start(bool announce = true);
    void Announce();       // ask now, if this run has not yet
    void Stop();           // UI thread; idempotent
    bool Running();
    int Version();         // bumps whenever a shown icon, its picture or its tooltip changes
    bool ProcessElevated();   // whether the dock runs as administrator

    struct IconView {
        uint64_t key = 0;            // stable while the icon exists
        std::wstring tip;            // tooltip to show; empty: none (the program shows its own pop-up)
        uint32_t imageVersion = 0;   // bumps when the picture changes
        bool glyph = false;          // one white or black colour: drawn in the dock's ink colour
    };
    std::vector<IconView> Shown();   // the icons for the dock, in Windows' order
    // the picture: premultiplied BGRA, top-down; false when the icon is gone
    bool Image(uint64_t key, uint32_t& imageVersion, int& w, int& h, std::vector<uint32_t>& pixels);

    enum class Mouse { LeftClick, LeftDoubleClick, RightClick, MiddleClick, Enter, Move, Leave };
    // Passes a mouse action to the icon's program the way Explorer does. anchor: the icon on screen
    // (programs ask for it to place their menus and pop-ups next to the icon).
    void Send(uint64_t key, Mouse m, POINT cursor, const RECT& anchor);

    // ---- hosting: another program's taskbar widget (FluentFlyout) on the dock
    // Such a program makes its widget a child window of the first "Shell_TrayWnd": the host's window
    // while it runs. Hosting on, the dock places that window (Place, every frame it changes) where its
    // slot for the widget is; the window is cut to what the widget shows, so it takes no clicks beside
    // it, its own pixels are see-through (only the widget shows on the glass), it takes no activation
    // (a click on the widget leaves the focus where it was, as on the dock), and it is kept out of
    // screen capture (the dock's glass, screenshots) like the dock.
    // only: the one hosted window to show - other programs' taskbar widgets that moved in stay cut away
    void SetHosting(bool on, HWND only = nullptr);
    // rect: screen px; raise: on top again; clip: screen px the window is cut to (the dock's monitor);
    // over: the dock's window - should it come up over the widget on its own, the widget goes back on top
    void Place(bool shown, const RECT& rect, bool raise = false, const RECT& clip = {}, HWND over = nullptr);
    struct Hosted {
        HWND window = nullptr;   // the first visible hosted window
        RECT box{};              // what the hosted windows show, in the host window's coordinates
        bool fitted = false;     // the hosted window spans the host window (laid out for its size)
    };
    Hosted HostedState();
    HWND Window();         // the host window while it runs
}
