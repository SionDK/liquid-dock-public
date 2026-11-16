#pragma once
#include "common.h"

// FluentFlyout (github.com/unchihugo/FluentFlyout) joined to the dock, as a module: while FluentFlyout
// is not running, or Settings::fluentFlyout is off, nothing here does anything and the dock is as before.
//   - Its taskbar widget and visualizer get a slot in the dock's glass, left of the start button. They
//     stay FluentFlyout's own window (its clicks, menus, tooltips, animations): FluentFlyout puts them in
//     the first "Shell_TrayWnd" it finds, which is the tray host's window (tray_host.h, hosting), and the
//     dock places that window over the slot.
//   - The slot is on the dock of the monitor FluentFlyout is set to ("taskbar widget monitor"): from its
//     settings file, and from where FluentFlyout actually puts its widget (fluent_logic.h).
//   - The slot follows FluentFlyout's own rules for showing the widget, "hide completely when no media is
//     playing" included: then it closes while nothing plays (the media sessions FluentFlyout reads).
//   - FluentFlyout makes its widget window again when the displays or the work area change: the slot
//     keeps its place for the new one meanwhile.
//   - Its flyouts (media, now playing, volume, lock keys) settle next to a shown dock instead of under it.
// Its .cpp files and what each holds: fluent_internal.h.
namespace fluentflyout {
    void Tick(double now);      // UI thread, every main-loop pass; cheap when nothing changes
    void InvalidateMonitors();  // the displays changed: read the monitors again on the next pass
    bool Present();             // FluentFlyout runs (its widget window was found, or a new one is awaited)
    bool WantsHost();           // its widget is on and a dock can hold it: the tray host must run, hosting
    HWND WidgetWindow();        // FluentFlyout's widget window (the one the host shows), if found

    struct Widget {
        HMONITOR monitor = nullptr;   // the dock that holds the slot (FluentFlyout's widget monitor)
        bool open = false;            // FluentFlyout shows something there now
        RECT box{};                   // what it shows, in the host window's coordinates
        bool fitted = false;          // FluentFlyout has laid it out for the host window's size
    };
    Widget WidgetState();
    int HostWidth(float scale);       // the host window's width; FluentFlyout lays its widget out in it

    // For the dock's menu: the monitor its widget goes to, FluentFlyout's number for it ("1", "2" ...;
    // 0 when not known), and opening FluentFlyout's settings window (to choose another monitor there).
    HMONITOR WidgetMonitor();
    int WidgetMonitorNumber();
    bool OpenSettings();

    // Each dock, every frame: places FluentFlyout's flyouts on its monitor next to it. True: a flyout has
    // just opened across the dock's glass - the dock goes on top of it until the flyout has moved clear.
    struct DockView {
        HMONITOR monitor = nullptr;
        Edge edge = Edge::Bottom;
        bool shown = false;                    // shown or coming up
        RECT bar{};                            // the glass, screen px, as when fully shown
        bool slot = false; RECT slotRect{};    // the widget's slot on this dock, screen px
        float scale = 1;
    };
    bool PlaceFlyouts(const DockView& v, double now);
    bool FlyoutAt(HMONITOR monitor, POINT p);  // the pointer is on a flyout placed next to this monitor's dock
    bool Busy();                               // a flyout is waiting to move or moving: frames at display rate
    void Shutdown();
}
