#pragma once
#include "common.h"
#include "gfx.h"
#include "capture.h"
#include "apps.h"
#include "window_preview.h"
#include "buttons.h"
#include "tray_host.h"
#include "fluent_flyout.h"
#include <shellapi.h>
#include "main.h"   // g_mainWnd, WM_APP_*

void OpenTaskManager();
UINT TaskbarCreatedMessage();   // Explorer announces a (new) taskbar with it (dock_window.cpp)
namespace sensorlogic { struct PanelFit; }   // sensor_logic.h (Dock::PanelRowsNow)
// a temperature in whole degrees ("62°", "62°C" with the unit, "--°" unknown) and its colour by severity (red hot,
// orange warm, otherwise `normal`): the sensor cell and its peek (dock_sensors.cpp, where they are) and the panel (dock_panel.cpp)
std::wstring TempStr(float t, bool unit = false);
D2D1_COLOR_F SevColor(int sev, D2D1_COLOR_F normal, float a);

#include "dock_shared.h"   // IconGapRatio, ThicknessRatio, StackButtons; g_settingsHold (the settings window holds the docks up)

// One dock per monitor. The implementation is split by concern:
//   dock.cpp           per-frame state (reveal, magnify, drag, bounce), model sync, window-in-front rules, region
//   dock_window.cpp    window and swap chain, monitor metrics, app bar, FluentFlyout's widget slot
//   dock_input.cpp     mouse messages, clicks, drag and drop (WndProc)
//   dock_layout.cpp    sizes, positions, u/v coordinates and hit testing
//   dock_render.cpp    glass and Direct2D drawing
//   dock_sensors.cpp   system monitor drawing: sensor and battery cells, overheat peek
//   dock_panel.cpp     the "PC 상태" panel: what fits in its room, its drawing
//   dock_controls.cpp  the small tray buttons, clicks and menus
//   dock_notify.cpp    the taskbar's tray icons
//   dock_record.cpp    the built-in screen recorder
class Dock {
public:
    Dock(HMONITOR mon, int index, bool primary);
    ~Dock();
    bool Create();
    void Update(double now, float dt, POINT cursor);
    void Render();
    bool Busy() const { return busy; }        // wants vsync-paced frames
    bool LiveGlass() const { return liveGlass; }   // shown at rest with a live backdrop: wants ~30 fps, not vsync
    bool FullscreenHere() const { return fullscreenHere; }   // last Update: a fullscreen app fills this monitor
    bool PanelWanted() const { return panelAlpha.target > 0.5f; }   // last Update: its sensor panel shows or is opening
    void RequestSnapshot() { snapshotPending = true; dirty = true; }
    static inline bool debugForceShow = false;
    static inline bool debugForcePanel = false;
    // set by the main loop before the docks' Update: nobody sees the screen (locked, a UAC prompt, the display
    // off). The sensor cell's changes wait meanwhile (one redraw when it is seen again).
    static inline bool screenUnseen = false;
    static void ResetNotifyBitmaps();          // tray icon pictures belong to the old device after a GPU reset
    // FluentFlyout's widget window went where a dock asked this frame (main hides it when none did)
    static inline bool widgetPlaced = false;
    static inline RECT widgetLastRect{};
    bool Relayout();                           // false: the window must be recreated instead
    void HotkeyReveal(double now);             // the reveal shortcut: show (or put away) this dock
    HWND hwnd = nullptr;
    HMONITOR monitor;
    int index;
    bool primary;

    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

private:
    struct Slot {
        std::shared_ptr<AppItem> app;   // null => start button
        bool ghost = false;             // removed from model; animating out
        Spring appear, gapBefore, shrink, press, peek;
        // bounce
        double bounceStart = 0; int bounceCount = 0; bool bounceLoop = false; bool bounceHiddenMode = false;
        float bounceOff = 0;
        // layout
        float u = 0, size = 0, u0 = 0, weight = 0;
    };
    enum HitKind { HitNone, HitSlot, HitTray, HitClock, HitSensor, HitPanel, HitBar, HitButton, HitNotify, HitBattery, HitKindCount };
    struct Hit {
        HitKind kind = HitNone; int slot = -1; ButtonId button = ButtonId::Count; int notify = -1;
        bool Is(ButtonId b) const { return kind == HitButton && button == b; }
    };
    static Hit ButtonHit(ButtonId b) { Hit h; h.kind = HitButton; h.button = b; return h; }

    // ---- small tray buttons (dock_controls.cpp): what a button does, one row per button. What it
    // looks like is in buttons.cpp (shared with the settings preview). Two buttons of a group share a
    // cell, split across the bar's thickness, when half the bar is still a comfortable click target.
    struct SmallButton {
        ButtonId id;
        std::wstring (Dock::* tooltip)() const;
        void (Dock::* click)();
        void (Dock::* rightClick)();       // nullptr: the dock menu
    };
    static const SmallButton kSmallButtons[kButtonCount];
    static const SmallButton& Button(ButtonId id) { return kSmallButtons[(int)id]; }
    // group 0: network + volume; group 1: the user's buttons, then settings
    static std::vector<ButtonId> Group(int group);
    std::wstring TipNetwork() const;
    std::wstring TipVolume() const;
    std::wstring TipDesktop() const;
    std::wstring TipTheme() const;
    std::wstring TipOled() const;
    std::wstring TipAwake() const;
    std::wstring TipScreenshot() const;
    std::wstring TipQuiet() const;
    std::wstring TipTrash() const;
    std::wstring TipDownloads() const;
    std::wstring TipAudio() const;
    std::wstring TipSff() const;
    std::wstring TipSettings() const;
    void ClickNetwork();
    void ClickVolume();
    void ClickDesktop();
    void ClickTheme();
    void ClickOled();
    void ClickAwake();
    void ClickScreenshot();
    void ClickSff();
    void ClickQuiet();
    void ClickTrash();
    void ClickDownloads();
    void ClickAudio();
    void ClickSettings();
    void MenuNetwork() { ShowTrayMenu(ButtonHit(ButtonId::Network)); }
    void MenuVolume() { ShowTrayMenu(ButtonHit(ButtonId::Volume)); }
    void MenuQuiet();
    void MenuTrash();
    void MenuDownloads();
    void MenuAudio();
    bool DropOnTrash(HDROP drop);

    // ---- layout (dock_layout.cpp; Metrics: dock_window.cpp)
    struct Cell { float u0 = 0, u1 = 0; int half = -1; float Mid() const { return (u0 + u1) * 0.5f; } };   // half -1: whole cell
    Cell cells[HitKindCount];              // tray, clock, sensor and battery
    Cell buttonCells[kButtonCount];        // the small buttons
    Cell& Cell_(ButtonId b) { return buttonCells[(int)b]; }
    const Cell& Cell_(ButtonId b) const { return buttonCells[(int)b]; }
    bool stackButtons = false;
    struct LayoutMetrics {
        float icon, padding, gap, thickness;
        float separator, trayButton, clock, sensor;
        float connectivity, utilities;     // the two small-button groups, after pairing
        float notify;                      // the taskbar's tray icons (Settings::trayIcons)
        float battery;                     // the battery cell after the sensor cell (0 without a battery)
        bool stack;
        float TrayWidth() const { return separator + trayButton + notify + connectivity + clock + sensor + battery + utilities; }
    };
    LayoutMetrics MeasureMetrics(float fitFactor, int thicknessPct = -1);
    float SmallButtonWidth(ButtonId b, const LayoutMetrics& m) const;
    float ArrangeGroup(int group, const LayoutMetrics& m, float x, bool assign);   // returns the end
    D2D1_RECT_F ButtonRect(ButtonId b, bool hit = false) const;
    float ButtonIconScale() const { return stackButtons ? 0.8f : 1.0f; }
    void Metrics();
    void ApplyFit(float dt);
    void Layout();
    Hit HitTestUV(float u, float v);
    void ScreenToUV(POINT p, float& u, float& v) const;
    void ClientToUV(int x, int y, float& u, float& v) const;
    Hit HitAtClient(LPARAM lp);            // a mouse message's point (dock_input.cpp)
    D2D1_POINT_2F W(float u, float v) const;
    D2D1_RECT_F RectUV(float u0, float u1, float v0, float v1) const;
    POINT ScreenPointUV(float u, float v) const;
    RECT  BarScreenRect() const;
    // A rectangle in window coordinates on the screen, outward to whole pixels (floor / ceil: what the window in
    // front must cover, the preview's anchor, a tray icon's) or cut toward zero ((LONG): the bar and the widget's
    // slot handed to FluentFlyout, BarScreenRect). The two differ by a pixel and are kept apart.
    RECT ScreenRectOut(const D2D1_RECT_F& r) const {
        return { winX + (LONG)std::floor(r.left), winY + (LONG)std::floor(r.top), winX + (LONG)std::ceil(r.right), winY + (LONG)std::ceil(r.bottom) };
    }
    RECT ScreenRectTrunc(const D2D1_RECT_F& r) const { return { winX + (LONG)r.left, winY + (LONG)r.top, winX + (LONG)r.right, winY + (LONG)r.bottom }; }
    // the bar across the edge where it is drawn now (slid toward the edge by hideOff while it hides); where it
    // sits when up (no hideOff: KeepInFront, the FluentFlyout view) is margin .. margin + barThick
    float BarV0() const { return margin - hideOff; }
    float BarV1() const { return margin + barThick - hideOff; }
    void SplitHalf(D2D1_RECT_F& r, int half) const;   // a cell shared by two (half 0 / 1; -1: the whole cell)
    // the top of a fully magnified icon + 10 px (UnpinLine and UpdateRegion: the same line, dock.h UnpinLine)
    float MagnifiedTop() const { return margin + pad + base * mag + 10 * scale; }
    // along the dock: on the sensor cell or just beside it (the bridge to its panel)
    bool NearSensorCell(float u) const { return u >= cells[HitSensor].u0 - 4 * scale && u <= cells[HitSensor].u1 + 4 * scale; }
    bool RevealShown() const { return reveal.x > 0.002f || reveal.target > 0.5f; }      // up, or on its way up
    bool PanelShown() const { return panelAlpha.x > 0.01f || panelAlpha.target > 0.5f; }   // the panel shows or opens
    float TextWidth(IDWriteTextFormat* f, const std::wstring& s);
    D2D1_SIZE_F TextSize(IDWriteTextFormat* f, const std::wstring& s);
    IDWriteTextFormat* ClockFont(bool date = false) const;
    const std::wstring& ClockTime() const;   // the clock's time text: "15:45" (settings clockFormat 1) or Windows' format
    enum class SensorText { Value, Line, Label };   // vertical value, horizontal "CPU 50°" line, vertical label
    IDWriteTextFormat* SensorFont(SensorText t) const;
    void ComputePanelRect();
    bool CursorInPanel() const;

    // ---- tray icons (dock_notify.cpp): the icons Windows shows on its taskbar, between the hidden-icons
    // chevron and network / volume, two to a cell across the bar like the small buttons
    struct NotifyCell {
        uint64_t key = 0; std::wstring tip; uint32_t imageVersion = 0; bool glyph = false; float u0 = 0, u1 = 0; int half = -1;
        float Mid() const { return (u0 + u1) * 0.5f; }
    };
    std::vector<NotifyCell> notifyCells;
    int notifyVersion = -1;
    double nextNotifySync = 0;
    uint64_t pressNotifyKey = 0;
    uint64_t notifyHoverKey = 0; double notifyHoverSince = 0, nextNotifyMove = 0; bool notifyEntered = false;
    POINT lastNotifyCursor{}; RECT notifyHoverAnchor{};
    uint64_t lastNotifyClickKey = 0; DWORD lastNotifyClickTick = 0; POINT lastNotifyClickPt{};
    void SyncNotifyIcons(double now);
    float ArrangeNotify(const LayoutMetrics& m, float x, bool assign);
    D2D1_RECT_F NotifyRect(int i) const;
    RECT NotifyScreenRect(int i) const;
    uint64_t NotifyKey(int i) const { return i >= 0 && i < (int)notifyCells.size() ? notifyCells[i].key : 0; }
    void DrawNotifyIcons(float vis);
    void NotifyHover(double now, POINT cursor);
    void NotifyClick(int i, trayhost::Mouse m);

    // ---- FluentFlyout's taskbar widget (fluent_flyout.h; dock_window.cpp): a slot at the start of the dock, left of the
    // start button, over which the tray host's window shows FluentFlyout's own widget window
    Spring widgetWidth;                    // the slot with the gap after it, along the dock (0: closed)
    float widgetU0 = 0, widgetU1 = 0;      // laid out (the slot without its gap)
    float appsU0 = 0;                      // where the apps begin (magnification starts there)
    bool widgetHere = false;               // this dock holds FluentFlyout's widget
    bool widgetOpen = false;               // it shows something
    bool widgetShowing = false;            // its window is shown over the slot (from when the slot has opened)
    bool raiseWidget = false;              // put its window back on top of the glass
    RECT widgetBox{};                      // what it shows, in the host window's coordinates
    void UpdateWidgetSlot();
    void PlaceWidget(bool fullscreen);
    void BringToFront();                   // the dock on top (topmost), FluentFlyout's widget above it
    // A window that comes to the front while the dock is up can land over it - a topmost one (Windows Terminal
    // "always on top") goes to the top of the topmost windows - and BringToFront runs only as the dock comes up:
    // when the window in front changes, and once more a little later, the dock goes back over an app's window
    // that is above it (Update; dock.cpp)
    void KeepInFront(double now, HWND front, bool fullscreen);
    static bool (*raiseOver)(HWND);        // which windows in front the dock goes back over (replaceable for tests)
    HWND lastFront = nullptr;              // the window in front at the last KeepInFront
    double frontRecheckAt = -1;            // the second look after it changed (-1: none due)
    int frontRaises = 0;                   // times the dock went back over the window in front (dock.log)

    // ---- frame (dock.cpp), window and app bar (dock_window.cpp), input (dock_input.cpp)
    void SyncSlots();
    // Update's steps (dock.cpp), run in this order once a frame. FrameState holds what a step worked out for the
    // steps after it, as it was then - read again, it could differ within the frame (the press safety lets a press
    // go after keep was taken; the dock told to come up after insideDock was).
    struct FrameState {
        double now; float dt; POINT cursor;
        bool wasVisible = false;                // FrameStart: reveal.x > 0.002f before this frame moved it
        float prevU = 0, prevV = 0;             // FrameStart: the pointer's u/v of the frame before
        bool inMonitor = false;                 // FrameStart
        HWND front = nullptr; HMONITOR fsMonitor = nullptr; bool fullscreen = false;   // FrameFullscreen
        bool insideDock = false, wasUp = false, keep = false, inPreview = false, inFlyout = false, alwaysShown = false;   // FrameShowHide
        bool anim = false;                      // FrameSprings on: something still moves
    };
    void FrameStart(FrameState& f);
    void FrameFullscreen(FrameState& f);
    void FrameBounceStarts(const FrameState& f);
    void FrameShowHide(FrameState& f);
    void FrameMagnify(const FrameState& f);
    void FramePressDrag(const FrameState& f);
    void FrameSprings(FrameState& f);
    void FramePlace(const FrameState& f);
    void FrameHover(const FrameState& f);
    void FramePanel(FrameState& f);
    void FrameTooltip(FrameState& f);
    void FrameCapture(const FrameState& f);
    void UpdateRegion();
    // Always-shown dock (auto-hide off, or a secondary monitor set to always show; never where OLED
    // mode is on): registered as an app bar so maximised windows stop at it, like the taskbar.
    bool AlwaysShown() const;
    void UpdateAppBar(bool want, double now = 0);
    void RemoveAppBar();                   // ABM_REMOVE; appBar off
    void OnTaskbarCreated();               // a new Explorer: register again (dock_window.cpp; WndProc)
    bool appBar = false; RECT appBarRect{};
    HWND appBarShell = nullptr;            // Explorer's taskbar the app bar was registered with (a new one knows none)
    double nextAppBarTry = 0;              // ABM_NEW failed (no Explorer yet), or a strip is registered again: when to ask (0: at once)
    bool appBarFailing = false;            // ABM_NEW has failed since the strip was last wanted or held (logged once)
    // The work area must leave the strip out (CheckAppBar): looked at a second after the strip is claimed and when the
    // work area changes (WM_SETTINGCHANGE). Not so: registered again, up to kAppBarRetries times until the strip is
    // wanted anew, claimed again or Explorer is new.
    void CheckAppBar(double now);
    RECT appBarGranted{};                  // the strip as the shell took it (ABM_SETPOS's answer)
    UINT appBarEdge = ABE_BOTTOM;          // ...on that edge
    double appBarCheckAt = 0;              // when to look at the work area (0: not due)
    bool workAreaChanged = false;          // WM_SETTINGCHANGE (SPI_SETWORKAREA) came: looked at shortly
    int appBarMisses = 0;                  // registered again since the work area last left the strip out
    static constexpr int kAppBarRetries = 3;
    void StartBounce(Slot& s, int count, bool loop, bool hiddenMode, double now);
    bool CreateDeviceResources();
    // Start, search, quick settings or the notification centre is in front on this monitor
    bool ShellPopupOpen() const;
    static bool (*isShellPopup)(HWND);     // which windows count (replaceable for tests)
    void Click(const Hit& h);
    void EndDrag(bool commit);
    void ClearPress();                     // the press (and its pressed look) is over, without a click

    // ---- menus (dock_controls.cpp)
    void ShowAppMenu(Slot& s);
    void ShowDockMenu();
    void ShowStartMenu();              // right-click on the start button: power and system
    void ShowTrayMenu(const Hit& h);   // network, volume, clock, sensor
    int  PopupMenu(HMENU m);
    void ToggleOled();

    // ---- drawing (dock_render.cpp; sensor and battery cells, peek: dock_sensors.cpp; panel: dock_panel.cpp)
    // Render's parts, in the order it draws them: the glass (Direct3D, before BeginDraw), then in one Direct2D
    // draw the app icons, the bar's contents (while it shows) with its hover highlight, what is drawn over them
    void DrawGlass(bool glassVisible);
    void DrawSlotIcons(ID2D1DeviceContext2* dc, float vis);
    void DrawBarContents(ID2D1DeviceContext2* dc, float vis);
    void DrawHoverHighlight(ID2D1DeviceContext2* dc, float v0, float v1, float vis);
    void DrawOverlays(bool glassVisible);
    void DrawIcon(Slot& s, float u, float vBottom, float size, float alpha);
    void DrawStartIcon(D2D1_RECT_F r, float alpha);
    void DrawTooltip(const std::wstring& text, float u, float vBase);
    void DrawShadowText(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, float u, float alpha);   // in the ink at u
    void DrawShadowText(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, float ink, float alpha, D2D1_COLOR_F color);
    void DrawSensorCell(float v0, float v1, float vis);
    void DrawBatteryCell(float v0, float v1, float vis);
    void DrawSensorPeek();
    void DrawPanel();
    struct PanelPaint;                     // DrawPanel's values, helpers and blocks (dock_panel.cpp)
    float InkAt(float u) const;
    D2D1_COLOR_F Ink(float u, float alpha) const;

    WindowPreview windowPreview;

    RECT mon{};          // monitor rect (physical px)
    int winX = 0, winY = 0, winW = 0, winH = 0;
    float len = 0, T = 0, scale = 1;
    float base = 54, pad = 8, gap = 6, margin = 6, barThick = 70, magRange = 140, mag = 1.7f;
    float baseNominal = 54, fit = 1;   // fit < 1 shrinks the dock when it would overflow the edge

    // state
    Spring reveal, magAmount, tooltipAlpha;
    float hoverU = 0, cursorU = -1, cursorV = 1e9;
    // Leaving the magnified apps sideways (Update, magnification): the end of the dock on that side - +1 the
    // tray end, -1 the start (FluentFlyout's widget), 0 none - stays at pinU while the apps shrink back, so
    // what is under the pointer there does not move; the other end comes in. pinBlend 1: held, eased to 0:
    // back at the centred place. pinOff: how far off that place the last layout put the dock.
    int pinSide = 0;
    float pinU = 0, pinOff = 0;
    Spring pinBlend;
    bool pinTold = false;                  // dock.log has this dock's "magnify:" line of this run
    double edgeSince = -1, outsideSince = -1;
    double hotkeyHoldUntil = 0;            // shown by the shortcut: stays until then (or while the pointer is on it)
    double lastCaptureWanted = -1e9, nextCaptureTry = 0;
    bool menuOpen = false;
    bool busy = false, dirty = true;
    bool liveGlass = false;                // see LiveGlass()
    bool fullscreenHere = false;           // see FullscreenHere()
    bool restingPrev = false;              // last frame's "resting" (Update): icon changes are paced then
    double nextRestingPoll = 0;            // capture pacing while resting
    bool glassKept = false;                // the last captured backdrop stays in use while the dock stays shown
                                           // (capture paused during a game elsewhere, or restarting)
    int lastStructure = -1, lastSysVersion = -1;
    Hit hover;                             // what the pointer is over (Update; none while hidden or dragging)
    std::wstring tooltipText; float tooltipU = 0, tooltipV = 0;

    std::vector<Slot> slots;
    Spring trailingGap;
    float sepU = 0, barU0 = 0, barU1 = 0, trayW = 0;
    float hideOff = 0;
    float edgeReserve = 0;                 // room kept free for magnification next to an offset dock
    // system monitor: cell at the end of the dock + glass popover panel
    Spring panelAlpha;
    double panelOutsideSince = -1;
    // panelW x panelH: the room the window was built with (physical px, screen orientation: the panel stands
    // upright on every edge). panelH holds the memory rows, the VRAM block and the drive rows the sensor
    // client expected then (PanelRoomHint); the panel itself is as high as what it shows (PanelRowsNow), at
    // most that. panelVramRoom: that room was made with the VRAM block (it then keeps its place whatever
    // comes later; in a window built without, the block shows only beside everything else - PanelRowsNow);
    // panelVramTempRoom: ...with its temperature row too. What the sensor settings hide has no room.
    float panelW = 0, panelH = 0;
    bool panelVramRoom = false, panelVramTempRoom = false;
    D2D1_RECT_F panelRect{};               // window coords
    // the "PC 상태" panel's rows (dock_panel.cpp): DIP sizes, and what fits in the room panelH leaves.
    // 450 DIP wide with two modules / drives a row on a horizontal dock (the reference picture); 320 DIP with
    // one a row on a vertical one, where the panel's width is the window's thickness T (450 would make T 150
    // DIP thicker than the old 300 DIP panel, the window a third larger; 320: 20 DIP, about 4 %).
    static float PanelWidthDip(bool vertical) { return vertical ? 320.f : 450.f; }
    static int PanelColumns(bool vertical) { return vertical ? 1 : 2; }
    static constexpr int kPanelMaxDimmRows = 4, kPanelMaxDriveRows = 6;   // the room a window is built with, at most
    // What it shows now (sensorlogic::PanelFit): dimmRows, driveRows (PanelColumns modules / drives a row); vram 1:
    // the VRAM block is shown (the GPU has memory of its own, and the room holds it), 2: ...with its temperature row;
    // drivesHidden: more than the room ("SSD 외 N개"), dimmsHidden ("RAM 외 N개": drives keep their rows first); height in DIP.
    sensorlogic::PanelFit PanelRowsNow() const;
    // The 그래프 power view's sparkline in a card (0 CPU, 1 GPU; dock_panel.cpp): its area and its line, as paths in
    // pixels from the card's graph box's top-left corner (drawn moved there), built again only when the sample
    // (g_sensors.powerVersion) or the box's size (w, h) changed - not while the panel slides in its fade or follows
    // the sensor cell. Device-independent (the factory's): kept across a GPU reset.
    struct PanelSpark { ComPtr<ID2D1PathGeometry> fill, line; int version = -1; float w = -1, h = -1; };
    PanelSpark panelSpark[2];
    int lastSensorVersion = -1;   // every sample: a redraw only while this dock's panel shows
    int lastCellVersion = -1;     // the sensor cell's look (g_sensors.cellVersion): a redraw while the screen is seen
    // overheat bounce of the sensor cell
    double sbStart = 0; int sbCount = 0; bool sbHidden = false; float sbOff = 0;
    Spring sbPeek;

    // input
    int pressSlot = -1; HitKind pressKind = HitNone; ButtonId pressButton = ButtonId::Count;
    POINT pressPt{};
    double pressUpSince = -1;              // a press under way: since when its primary button is seen up (-1: held)
    bool startInFrontAtPress = false;   // Start was open when the start button went down: the click closes it
    bool dragging = false;
    int dropIndex = -1; bool dropRemove = false;
    // From the screen edge: the top of the strip the dock's icons take while none is magnified (UpdateRegion:
    // the window's region ends there, rounded up to 32 px).
    float IconStripTop() const { return margin + barThick + 14 * scale; }
    // A pinned app dragged past this line is unpinned when dropped (dock.cpp): where the dragged icon, drawn
    // around the pointer, is wholly cut off by that region - what looks like off the dock is off it. Never
    // below the top of a magnified icon + 10 px either, so a press on one's upper part (the icons shrink back
    // as the drag starts) cannot begin a drag already past it. Not the window's thickness T: that holds the
    // magnified icons and the "PC 상태" panel (537 px at 100% with the VRAM block), far above anything
    // the dock shows while an icon is dragged - an icon gone from sight still only moved when dropped there.
    float UnpinLine() const;

    // region
    RECT curRegion{ -1, -1, -1, -1 };
    BOOL cloaked = FALSE;   // DWM-cloaked while nothing of the dock is up (UpdateRegion)

    // gpu
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<IDCompositionTarget> compTarget;
    ComPtr<IDCompositionVisual> visual;
    RTTexture backdrop, halfA, halfB;
    bool backdropDirty = false;
    MonitorCapture capture;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Effect> darken;
    bool clearedWhileHidden = false;
    // backdrop luminance probe -> adaptive ink (0 = light ink, 1 = dark ink)
    RTTexture lumRT;
    ComPtr<ID3D11Texture2D> lumStage[2];
    int lumFrame = 0;
    float lumCol[16] = {}, inkTarget[16] = {}, inkCol[16] = {};

    bool snapshotPending = false;
    void SaveSnapshot();

public:
    // built-in screen recorder (normal recorders can't see the dock: it is excluded from capture)
    void StartRecording(RECT cropMonitorPx, float outScale, bool append = false);
    // a recording outlives the dock windows being rebuilt (position change): carried over by BuildDocks
    static inline bool carryRecording = false;
    static inline RECT carryCrop{};
    static inline float carryScale = 0.5f;
    static inline int carryFrames = 0;
    void StopRecording();
    void SuspendRecording();
    void RecordTick(double now);
    bool Recording() const { return rec.recording; }
private:
    // The recorder's state (dock_record.cpp). Update and Render look at recording, dockFrame (Render copies the
    // dock's frame there) and fullTex (the capture copies the whole monitor there) only.
    struct DockRecorder {
        bool recording = false;
        double next = 0;                   // when the next frame is due
        int frames = 0, w = 0, h = 0, slot = 0;   // frames written; the output's size; readback slot (two frames late)
        RECT crop{};                       // monitor px
        FILE* file = nullptr;              // frames.raw
        RTTexture dockFrame, fullTex, rt;  // rt: the output frame
        ComPtr<ID3D11Texture2D> stage[3];
        ComPtr<ID2D1Bitmap1> target;
    };
    DockRecorder rec;
};
