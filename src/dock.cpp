#include "dock.h"
#include "system.h"
#include "sensors.h"
#include <dwmapi.h>

void Dock::HotkeyReveal(double now) {
    if (reveal.target > 0.5f) { reveal.target = 0; hotkeyHoldUntil = 0; edgeSince = -1; return; }   // pressed again: away
    hotkeyHoldUntil = now + 3;
    reveal.target = 1; outsideSince = -1;
    lastCaptureWanted = now;
    BringToFront();
    dirty = true;
}

// ---------------------------------------------------------------- model sync

void Dock::SyncSlots() {
    if (lastStructure == g_apps.structureVersion && !slots.empty()) return;
    lastStructure = g_apps.structureVersion;

    // An app opens its first window or closes its last one while an icon is pressed or dragged: that icon
    // stays pressed (or in hand), found again by its app below.
    const bool pressed = pressSlot >= 0 && pressSlot < (int)slots.size();
    const auto pressedApp = pressed ? slots[pressSlot].app : nullptr;   // nullptr: the start button
    std::vector<Slot> old;
    old.swap(slots);
    std::vector<bool> used(old.size(), false);

    // start button always first
    {
        Slot st; bool had = false;
        for (size_t i = 0; i < old.size(); ++i) if (!old[i].app) { st = old[i]; used[i] = true; had = true; break; }
        if (!had) { st.appear.snap(1); st.shrink.snap(1); }
        slots.push_back(st);
    }
    for (auto& a : g_apps.items) {
        bool found = false;
        for (size_t i = 0; i < old.size(); ++i) {
            if (!used[i] && old[i].app == a) {
                slots.push_back(old[i]); used[i] = true; found = true;
                slots.back().ghost = false;
                slots.back().appear.target = 1;
                break;
            }
        }
        if (!found) {
            Slot s; s.app = a;
            s.appear.x = 0; s.appear.target = 1;
            s.shrink.snap(1);
            slots.push_back(s);
        }
    }
    // keep removed items as ghosts that shrink away, next to their old neighbour
    for (size_t i = 0; i < old.size(); ++i) {
        if (used[i] || !old[i].app) continue;
        Slot g = old[i];
        g.ghost = true; g.appear.target = 0;
        size_t pos = 1;
        for (size_t j = i; j-- > 0;) {
            auto it = std::find_if(slots.begin(), slots.end(), [&](const Slot& s) { return s.app && s.app == old[j].app; });
            if (it != slots.end()) { pos = (it - slots.begin()) + 1; break; }
            if (!old[j].app) break;
        }
        slots.insert(slots.begin() + std::min(pos, slots.size()), g);
    }
    pressSlot = -1;
    if (pressed)
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].app == pressedApp && !slots[i].ghost) { pressSlot = (int)i; break; }
    if (dragging && pressSlot < 0) EndDrag(false);   // the dragged app is gone from the dock: nothing to drop
    dirty = true;
}

namespace {
// Not cloaked - shown, not put away by DWM (another virtual desktop, a closed shell pop-up) - or DWM cannot say.
bool NotCloaked(HWND w) {
    DWORD cloaked = 0;
    return FAILED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || !cloaked;
}

// The shell's own pop-ups (shell_front.h): the taskbar comes up with them, so the dock does too - one that shows: a closed
// one is a cloaked window of its program that can keep the front with nothing on the screen (SearchHost's after the
// monitors came back on, a two-monitor PC 2026-10-30: both docks held up for it). The cloak asked only of a shell window.
bool ShellPopupWindow(HWND w) {
    if (!IsShellPopupWindow(w)) return false;
    return NotCloaked(w);
}
}

bool (*Dock::isShellPopup)(HWND) = ShellPopupWindow;

namespace {
// The settings window holds the docks up only while it can be seen: open but minimized, or cloaked on another
// virtual desktop (or by however Win+D puts away a window without a minimize box), it held every dock up with
// nothing on the screen to say why (2026-11-06). Asked only while it is open.
bool SettingsHold() {
    if (!g_settingsHold.open) return false;
    HWND w = g_settingsHold.wnd;
    if (!w) return true;
    if (!IsWindowVisible(w) || IsIconic(w)) return false;
    return NotCloaked(w);
}
}

bool Dock::ShellPopupOpen() const {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == hwnd) return false;
    if (MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) != monitor) return false;
    return isShellPopup(fg);
}

namespace {
bool RectCovers(const RECT& r, const RECT& m) { return r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom; }

// A maximized window in front, without a caption: fullscreen only as a browser leaves it when F11 (or a page's
// fullscreen video) comes over a maximized window - still maximized, its caption and resize frame gone, the
// window and its client area exactly the monitor (probe at 150%: caption 0 maximised 1 rect = the monitor). A
// maximized window of its own keeps its resize frame and hangs over the monitor by it (Chrome 11 px past each edge
// at 150%, about 8 px at 100%); a self-drawn one that sizes itself to the work area - the whole monitor where
// an auto-hide dock hid the taskbar - keeps the frame too, or leaves its client area a pixel or two short for
// the auto-hide bar; a maximized WS_POPUP Windows itself puts on the monitor exactly (Qt or WPF frameless
// windows), and nothing tells it from a fullscreen one: those stay windows the dock comes up over.
bool ZoomedFullscreen(HWND w, const RECT& r, const RECT& monitor, LONG_PTR style) {
    if (style & (WS_POPUP | WS_THICKFRAME)) return false;
    if (!EqualRect(&r, &monitor)) return false;
    WINDOWINFO wi = { sizeof(wi) };
    return GetWindowInfo(w, &wi) && RectCovers(wi.rcClient, monitor);
}

// The monitor a fullscreen app (a game, a video) fills when the window in front is one, else nullptr.
// rect: the window's rectangle - spanned across monitors (a game or a video wall over two screens), it
// fills every monitor it covers, not only the one Windows names for it (the larger share).
HMONITOR FullscreenMonitor(HWND dockWindow, HWND fg, RECT* rect = nullptr) {
    if (!fg || fg == dockWindow) return nullptr;
    wchar_t cls[64] = {}; GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return nullptr;
    HMONITOR m = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
    MONITORINFO mi = { sizeof(mi) };
    if (!m || !GetMonitorInfoW(m, &mi)) return nullptr;
    RECT r; GetWindowRect(fg, &r);
    if (!RectCovers(r, mi.rcMonitor)) return nullptr;
    LONG_PTR st = GetWindowLongPtrW(fg, GWL_STYLE);
    if ((st & WS_CAPTION) == WS_CAPTION) return nullptr;
    if (IsZoomed(fg) && !ZoomedFullscreen(fg, r, mi.rcMonitor, st)) return nullptr;
    if (rect) *rect = r;
    return m;
}

// w lies above the dock's window (among the windows over it in the z-order).
bool AboveDock(HWND w, HWND dock) {
    for (HWND h = GetWindow(dock, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV))
        if (h == w) return true;
    return false;
}

// The programs whose windows in front the dock goes back over: an app's. Not Explorer's (Alt+Tab, Task view, the
// desktop, the taskbar) and not the dock's own (its menus, the preview). The shell's pop-ups (Start, search, quick
// settings, the calendar), which the dock comes up with, are left out by KeepInFront (isShellPopup).
bool RaiseOverWindow(HWND w) {
    DWORD pid = 0, shellPid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return false;
    if (HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &shellPid);
    return !(pid && pid == shellPid);
}

constexpr double kFrontRecheck = 0.3;   // s: the second look after the window in front changed
}

bool (*Dock::raiseOver)(HWND) = RaiseOverWindow;

// 2026-11-16 (mod.32): Windows Terminal set to "always on top" in fullscreen, the dock up and its
// "show desktop" button pressed twice - the second press brought the terminal back to the front, to the top of the
// topmost windows: over the dock, which stayed "up" under it (the pointer still on its place) for as long as the
// pointer stayed there, with nothing in dock.log. Looked at only when the window in front changes (the change
// itself and kFrontRecheck later, as its z-order can follow the activation a little later) and only for a dock
// that is up: a hidden one is raised as it comes up.
void Dock::KeepInFront(double now, HWND front, bool fullscreen) {
    bool look = false;
    if (front != lastFront) { lastFront = front; frontRecheckAt = now + kFrontRecheck; look = true; }
    else if (frontRecheckAt >= 0 && now >= frontRecheckAt) { frontRecheckAt = -1; look = true; }
    if (!look || !front || front == hwnd || reveal.target < 0.5f || fullscreen) return;
    // Only a window over the whole bar - where the bar sits when up - as the terminal (fullscreen or maximized):
    // a smaller one over part of it (a program's own pop-up opened from a tray icon, a picture-in-picture video
    // the user put there) stays over the dock, as before and as it would over the taskbar.
    const auto b = RectUV(barU0, barU1, margin, margin + barThick);
    const RECT bar = ScreenRectOut(b);
    RECT r{};
    if (!GetWindowRect(front, &r) || !RectCovers(r, bar)) return;
    if (!AboveDock(front, hwnd) || !raiseOver(front) || isShellPopup(front)) return;
    BringToFront();
    ++frontRaises;
    if (frontRaises <= 3 || frontRaises % 100 == 0) {
        wchar_t cls[64] = {}; GetClassNameW(front, cls, 64);
        Log(L"front: dock %d was under the window in front (%s, came to the front over it) - raised again (%d %s)",
            index, cls, frontRaises, frontRaises == 1 ? L"time" : L"times");
    }
}

namespace {
// A bounce is ballistic hops of kHop seconds each (an app's and the sensor cell's): which hop it is in (the whole
// part; the fraction: how far through it), and the height of the parabola there.
constexpr double kHop = 0.56;
double HopPhase(double now, double start) { return (now - start) / kHop; }
float HopOffset(double phase, float height) {
    float fr = float(phase - std::floor(phase));
    return height * 4 * fr * (1 - fr);
}
}

void Dock::StartBounce(Slot& s, int count, bool loop, bool hiddenMode, double now) {
    if (s.bounceCount > 0 || s.bounceLoop) {
        // already bouncing: extend
        if (loop) s.bounceLoop = true;
        else {
            double phase = HopPhase(now, s.bounceStart);
            s.bounceCount = std::max(s.bounceCount, (int)std::ceil(phase) + count - 1);
        }
        s.bounceHiddenMode = s.bounceHiddenMode || hiddenMode;
        return;
    }
    s.bounceStart = now;
    s.bounceCount = count;
    s.bounceLoop = loop;
    s.bounceHiddenMode = hiddenMode;
}

// ---------------------------------------------------------------- per-frame update

// One frame, in steps (dock.h FrameState), always all of them in this order. What a step works out is kept in
// the frame for the steps after it, as it was then: keep, taken before the press safety (FramePressDrag) may let
// the press go, still holds the capture on (FrameCapture); insideDock, taken before the dock is told to come up,
// says the same to every step of the frame.
void Dock::Update(double now, float dt, POINT cursor) {
    FrameState f{ now, dt, cursor };
    FrameStart(f);
    FrameFullscreen(f);
    FrameBounceStarts(f);
    FrameShowHide(f);
    FrameMagnify(f);
    FramePressDrag(f);
    FrameSprings(f);
    FramePlace(f);
    FrameHover(f);
    FramePanel(f);
    FrameTooltip(f);
    FrameCapture(f);
    UpdateRegion();
}

// fonts, the model, the fit, the pointer in u/v, what the system and the sensors changed
void Dock::FrameStart(FrameState& f) {
    const auto& now = f.now; const auto& dt = f.dt; const auto& cursor = f.cursor;
    auto& wasVisible = f.wasVisible; auto& prevU = f.prevU; auto& prevV = f.prevV; auto& inMonitor = f.inMonitor;
    g_gfx.EnsureFonts(scale);   // text formats for this monitor's scale
    SyncSlots();
    SyncNotifyIcons(now);
    {
        float before = fit;
        ApplyFit(dt);
        if (std::fabs(before - fit) > 0.0005f) dirty = true;
    }
    wasVisible = reveal.x > 0.002f;
    prevU = cursorU; prevV = cursorV;
    ScreenToUV(cursor, cursorU, cursorV);
    inMonitor = PtInRect(&mon, cursor) != 0;
    if (!inMonitor) { cursorV = 1e9f; }
    if (g_sys.version != lastSysVersion) { lastSysVersion = g_sys.version; dirty = true; }
    // sensors: a sample every 2 s (every second while a panel is open) - the panel shows every one; the cell
    // only when it would look different (cellVersion), and not while nobody sees the screen (left unconsumed:
    // one redraw when it is seen again). A resting always-shown dock used to redraw (glass, text, Present)
    // for every sample, 1,800 times an hour, mostly for the same text.
    if (g_sensors.version != lastSensorVersion) {
        lastSensorVersion = g_sensors.version;
        if (PanelShown()) dirty = true;
    }
    if (g_sensors.cellVersion != lastCellVersion && !screenUnseen) { lastCellVersion = g_sensors.cellVersion; dirty = true; }
}

// A fullscreen app (a game, a video) in front: on a monitor it covers the dock stays away (spanned
// across monitors, it covers more than the one Windows names for it); on another monitor an
// always-shown dock here pauses its glass (capture lifetime, below).
void Dock::FrameFullscreen(FrameState& f) {
    auto& front = f.front; auto& fsMonitor = f.fsMonitor; auto& fullscreen = f.fullscreen;
    RECT fsRect{};
    front = GetForegroundWindow();
    fsMonitor = FullscreenMonitor(hwnd, front, &fsRect);
    fullscreen = monitor && fsMonitor && (fsMonitor == monitor || RectCovers(fsRect, mon));
    fullscreenHere = fullscreen;
}

// bounces that start this frame: overheat, notifications, launches
void Dock::FrameBounceStarts(const FrameState& f) {
    const auto& now = f.now; const auto& fullscreen = f.fullscreen;
    // the quiet button holds back notification and overheat bounces (launch hops still show)
    const bool quiet = features::Quiet(now);

    // overheat: the sensor cell bounces once (3 hops) on the primary monitor
    if (primary && !quiet && g_sensors.alertPending && sbCount == 0) {
        sbStart = now; sbCount = 3; sbHidden = reveal.target < 0.5f;
        if (!fullscreen) lastCaptureWanted = now;   // the hop itself draws no glass: never capture a game for it
    }

    // notifications bounce on the primary monitor only
    if (primary && !quiet) {
        for (auto& ev : g_apps.attentionEvents) {
            for (auto& s : slots) {
                if (s.app && s.app->id == ev.id && !s.ghost) {
                    if (!fullscreen) {
                        StartBounce(s, 3, false, reveal.target < 0.5f, now);
                        if (s.bounceHiddenMode) lastCaptureWanted = now;
                    }
                }
            }
        }
    }
    // launch feedback bounces until the app shows a window
    for (auto& s : slots) {
        if (!s.app) continue;
        bool launching = s.app->launchStarted > 0 && s.app->launchDock == index;
        if (launching && !s.bounceLoop) StartBounce(s, 0, true, false, now);
        if (!launching && s.bounceLoop) {
            double phase = HopPhase(now, s.bounceStart);
            s.bounceLoop = false;
            s.bounceCount = std::max(1, (int)std::ceil(phase));
        }
    }
}

void Dock::FrameShowHide(FrameState& f) {
    const auto& now = f.now; const auto& cursor = f.cursor; const auto& inMonitor = f.inMonitor; const auto& front = f.front;
    const auto& fullscreen = f.fullscreen;
    auto& insideDock = f.insideDock; auto& wasUp = f.wasUp; auto& keep = f.keep; auto& inPreview = f.inPreview; auto& inFlyout = f.inFlyout;
    auto& alwaysShown = f.alwaysShown;
    // ---- show / hide
    bool atEdge = inMonitor && cursorV < 2.0f;
    float magExtent = base * (mag - 1) * magAmount.x;
    insideDock = inMonitor && reveal.target > 0.5f &&
        ((cursorU >= barU0 - 14 * scale && cursorU <= barU1 + 14 * scale &&
          cursorV <= margin + barThick + std::max(magExtent, 0.0f) + 12 * scale) ||
         CursorInPanel() ||
         (panelAlpha.target > 0.5f && NearSensorCell(cursorU) && cursorV <= margin + barThick + 16 * scale));   // bridge between cell and panel
    wasUp = reveal.target > 0.5f;   // the dock insideDock knows: one told to come up below is not "inside" until the next frame
    // the Windows key (Start), Win+S, Win+A, Win+N: the dock comes up with them, like the taskbar would
    bool shellPopup = g_settings.showWithStart && ShellPopupOpen();
    const bool settingsHold = SettingsHold();
    keep = menuOpen || dragging || pressSlot >= 0 || debugForceShow || settingsHold || now < hotkeyHoldUntil || shellPopup;
    inPreview = windowPreview.Contains(cursor);
    inFlyout = fluentflyout::FlyoutAt(monitor, cursor);   // FluentFlyout's flyout, settled next to this dock
    if (debugForceShow || settingsHold) reveal.target = 1;
    alwaysShown = AlwaysShown();
    UpdateAppBar(alwaysShown, now);

    if (alwaysShown) {
        // always shown: only a fullscreen app (a game, a video) on this monitor puts the dock away
        if (fullscreen && !keep) reveal.target = 0;
        else if (reveal.target < 0.5f) {
            reveal.target = 1;
            BringToFront();
        }
        outsideSince = edgeSince = -1;
    } else if (reveal.target < 0.5f) {
        if (shellPopup) {
            Log(L"shell pop-up in front: dock %d shown", index);
            reveal.target = 1;
            outsideSince = -1;
            BringToFront();
        } else if (atEdge) {
            if (edgeSince < 0) edgeSince = now;
            if (!fullscreen) lastCaptureWanted = now;        // pre-warm capture
            if (now - edgeSince >= g_settings.showDelay && !fullscreen) {
                reveal.target = 1;
                outsideSince = -1;
                BringToFront();
            }
        } else edgeSince = -1;
        // Pre-warm near the edge - but not under a fullscreen app: the dock will not come up there, and
        // the capture would copy every frame of the game (edge-scrolling games keep the pointer there).
        if (inMonitor && cursorV < 60 * scale && !fullscreen) lastCaptureWanted = now;
    } else {
        // The edge that revealed the dock also keeps it open, including corners.
        if (keep || (!fullscreen && (insideDock || atEdge || inPreview || inFlyout))) outsideSince = -1;
        else {
            if (outsideSince < 0) outsideSince = now;
            if (now - outsideSince >= g_settings.hideDelay) { reveal.target = 0; edgeSince = -1; }
        }
    }
    KeepInFront(now, front, fullscreen);   // a window that came to the front over the dock while it is up
}

void Dock::FrameMagnify(const FrameState& f) {
    const auto& insideDock = f.insideDock; const auto& inPreview = f.inPreview; const auto& inFlyout = f.inFlyout; const auto& fullscreen = f.fullscreen;
    const auto& wasUp = f.wasUp;
    // ---- magnification
    float appEnd = sepU;
    bool overApps = insideDock && cursorU >= appsU0 && cursorU <= appEnd;
    bool overBar = overApps && cursorV <= margin + barThick + 2 * scale;
    bool overMag = overApps && cursorV <= margin + pad + base * (1 + (mag - 1) * magAmount.x) + 6 * scale;
    const bool canMagnify = mag > 1.01f && reveal.x > 0.55f && !dragging && !menuOpen;
    // Beside the apps - on the tray and the buttons after them, or FluentFlyout's widget before them - the apps
    // shrink back. The dock is centred, so that alone would slide the clock, the tray icons or the widget away
    // from the pointer in the middle of an attempted click: the end of the dock on that side is held where it
    // is instead (Layout), and only the other end comes in. Taken when the pointer comes off the magnified apps
    // or arrives there while they still shrink - once, not every frame, or rounding would walk the end away.
    const int beside = !insideDock || inPreview ? 0 : cursorU > appEnd ? 1 : cursorU < appsU0 ? -1 : 0;
    if (canMagnify && beside != 0 && magAmount.x > 0.002f && (magAmount.target > 0.5f || beside != pinSide)) {
        pinSide = beside;
        pinU = beside > 0 ? barU1 : barU0;   // as laid out last frame: holding it there moves nothing
        pinBlend.snap(1);
    }
    if (canMagnify && (overBar || (magAmount.target > 0.5f && overMag))) {
        magAmount.target = 1;
        hoverU = cursorU;
    } else if (!(inPreview && !fullscreen && !dragging && !menuOpen)) magAmount.target = 0;
    if (pinSide != 0) {
        const bool away = reveal.x < 0.002f && reveal.target < 0.5f;
        const bool onDock = insideDock || inPreview || inFlyout || menuOpen || dragging || pressSlot >= 0;
        const bool holding = pinBlend.target > 0.5f;
        // The dock stays up without the pointer (always shown, the pointer along the screen edge, Start open): it
        // eases back to its place. One about to be put away keeps the end until it is hidden - no slide on the way out.
        // (wasUp: on the frame a dock on its way out is told to come up again, insideDock cannot say yet whether the
        // pointer is on it - eased for that one frame, the end would twitch under a pointer that is.)
        const bool easeBack = !away && !onDock && holding && wasUp && reveal.target > 0.5f && outsideSince < 0;
        if ((away && holding) || easeBack) {
            // dock.log: the first time in a run (that it works here), and whenever the held end did move
            const float moved = (pinSide > 0 ? barU1 : barU0) - pinU;
            if (!pinTold || std::fabs(moved) > 0.05f)
                Log(L"magnify: dock %d held its %ls end at %.1f while the apps shrank (it moved %.2f px, the dock %.0f px off centre) - let go: %ls",
                    index, pinSide > 0 ? L"tray" : L"start", pinU, moved, pinOff, away ? L"hidden" : L"the pointer gone");
            pinTold = true;
        }
        if (away) { pinSide = 0; pinBlend.snap(0); }   // put away: it comes up centred
        else if (magAmount.target > 0.5f) {
            // Magnifying again: the apps grow out from the end the dock is off toward until it is where a centred
            // dock would be (Layout stops it there) - then nothing is held any more. If the dock got shorter while the
            // end was held (an app closed, FluentFlyout's slot shut), fully grown it does not reach that place: it
            // eases back to it instead of staying off centre for as long as the pointer is on the apps.
            if (std::fabs(pinOff) < 0.05f) { pinSide = 0; pinBlend.snap(0); }
            else if (!magAmount.settled()) { pinSide = pinOff > 0 ? 1 : -1; pinU = pinSide > 0 ? barU1 : barU0; pinBlend.snap(1); }
            else if (holding) pinBlend.target = 0;   // let go below, once pinBlend has settled
        } else if (onDock && !holding) { pinU = pinSide > 0 ? barU1 : barU0; pinBlend.snap(1); }   // easing back stops where it is: nothing slides under the pointer
        else if (easeBack) pinBlend.target = 0;
    }
}

void Dock::FramePressDrag(const FrameState& f) {
    const auto& now = f.now; const auto& cursor = f.cursor;
    // ---- press and drag
    // The primary button, as the drag reads it: GetAsyncKeyState reports the physical buttons, and with
    // Windows' "swap buttons" setting (a left-handed mouse) the primary one is the physical right one.
    // A press or drag whose button-up never reaches the dock (the capture went without a WM_CAPTURECHANGED,
    // e.g. to another desktop: the lock screen, a UAC prompt) must not hold the dock up and busy: from the
    // first frame the button is seen up the press can no longer become a drag (a drag elsewhere on the
    // screen later), and 2 s on it is over. A button-up still in the queue arrives long before that.
    bool primaryHeld = false;
    if (pressKind != HitNone || dragging) {
        primaryHeld = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
        if (!primaryHeld && pressUpSince < 0) pressUpSince = now;
        if (pressUpSince >= 0 && now - pressUpSince > 2.0) {
            Log(L"input: dock %d - the button-up of a %ls never came: given up", index, dragging ? L"drag" : L"press");
            EndDrag(false);
            ClearPress();
        }
    } else pressUpSince = -1;
    if (pressSlot >= 0 && !dragging && pressUpSince < 0 && pressSlot < (int)slots.size() && slots[pressSlot].app && primaryHeld) {
        float dx = float(cursor.x - pressPt.x), dy = float(cursor.y - pressPt.y);
        if (dx * dx + dy * dy > 36 * scale * scale) {
            dragging = true;
            slots[pressSlot].shrink.target = 0;
            slots[pressSlot].press.target = 0;
            magAmount.target = 0;
        }
    }
    if (dragging && pressSlot >= 0 && pressSlot < (int)slots.size()) {
        auto& ds = slots[pressSlot];
        dropRemove = ds.app->pinned && cursorV > UnpinLine();   // off the dock as it looks (dock.h)
        int k = 0, count = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            if ((int)i == pressSlot || slots[i].ghost || !slots[i].app) continue;
            if (slots[i].u < cursorU) k++;
            count++;
        }
        dropIndex = k;
        int j = 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            if ((int)i == pressSlot || slots[i].ghost || !slots[i].app) { slots[i].gapBefore.target = 0; continue; }
            slots[i].gapBefore.target = (!dropRemove && j == k) ? 1.0f : 0.0f;
            j++;
        }
        trailingGap.target = (!dropRemove && k == count) ? 1.0f : 0.0f;
    }
}

void Dock::FrameSprings(FrameState& f) {
    const auto& now = f.now; const auto& dt = f.dt;
    auto& anim = f.anim;
    // ---- springs
    UpdateWidgetSlot();
    reveal.step(dt, 230, reveal.target > 0.5f ? 0.72f : 1.0f);
    magAmount.step(dt, 380, 1.0f);
    pinBlend.step(dt, 380, 1.0f);   // easing back to the centred place: critically damped like the magnification, no swing past it
    if (pinSide != 0 && pinBlend.target == 0 && (pinBlend.settled() || std::fabs(pinOff) < 0.05f)) { pinSide = 0; pinBlend.snap(0); }
    trailingGap.step(dt, 320, 0.9f);
    widgetWidth.step(dt, 300, 0.92f);
    if (widgetWidth.target == 0 && widgetWidth.x < 0.5f) widgetWidth.snap(0);
    anim = !reveal.settled() || !magAmount.settled() || !pinBlend.settled() || !trailingGap.settled() || !widgetWidth.settled(0.25f) || dragging;
    for (auto& s : slots) {
        s.appear.step(dt, 200, 0.72f);
        s.gapBefore.step(dt, 320, 0.9f);
        s.shrink.step(dt, 320, 0.9f);
        s.press.step(dt, 700, 1.0f);
        s.peek.step(dt, 170, 0.8f);
        // bounce: ballistic hops, kHop (0.56 s) each
        if (s.bounceCount > 0 || s.bounceLoop) {
            double phase = HopPhase(now, s.bounceStart);
            if (!s.bounceLoop && phase >= s.bounceCount) {
                s.bounceCount = 0; s.bounceOff = 0; s.peek.target = 0; s.bounceHiddenMode = false;
            } else {
                float hgt = base * (s.bounceLoop ? 0.5f : 0.9f);   // launch: small hops, notification: big ones
                s.bounceOff = HopOffset(phase, hgt);
                s.peek.target = (s.bounceHiddenMode && reveal.target < 0.5f) ? 1.0f : 0.0f;
            }
            anim = true;
        }
        if (!s.appear.settled() || !s.gapBefore.settled() || !s.shrink.settled() || !s.press.settled() || !s.peek.settled()) anim = true;
    }
    // sensor cell bounce
    if (sbCount > 0) {
        double phase = HopPhase(now, sbStart);
        if (phase >= sbCount) { sbCount = 0; sbOff = 0; sbPeek.target = 0; sbHidden = false; }
        else {
            sbOff = HopOffset(phase, base * 0.9f);
            sbPeek.target = (sbHidden && reveal.target < 0.5f) ? 1.0f : 0.0f;
        }
        anim = true;
    }
    sbPeek.step(dt, 170, 0.8f);
    if (!sbPeek.settled()) anim = true;
    // drop finished ghosts
    for (size_t i = slots.size(); i-- > 0;) {
        if (slots[i].ghost && slots[i].appear.x < 0.01f && slots[i].appear.target == 0) {
            slots.erase(slots.begin() + i);
            if (pressSlot == (int)i) pressSlot = -1; else if (pressSlot > (int)i) pressSlot--;
        }
    }

    // adaptive ink with hysteresis, eased
    for (int i = 0; i < 16; ++i) {
        if (lumCol[i] > 0.52f) inkTarget[i] = 1; else if (lumCol[i] < 0.42f) inkTarget[i] = 0;
        float k = std::min(1.0f, dt * 7);
        float nv = inkCol[i] + (inkTarget[i] - inkCol[i]) * k;
        if (std::fabs(nv - inkCol[i]) > 0.002f) anim = true;
        inkCol[i] = nv;
    }
}

// the layout of this frame (hideOff first: Layout and the rectangles after it read it), the widget, FluentFlyout's flyouts
void Dock::FramePlace(const FrameState& f) {
    const auto& now = f.now; const auto& fullscreen = f.fullscreen;
    hideOff = (1 - reveal.x) * (margin + barThick + 34 * scale);
    Layout();
    ComputePanelRect();
    PlaceWidget(fullscreen);
    {   // FluentFlyout's flyouts on this monitor settle next to the dock while it is up
        fluentflyout::DockView view;
        view.monitor = monitor;
        view.edge = g_settings.edge;
        view.shown = reveal.target > 0.5f && !fullscreen;
        view.scale = scale;
        auto bar = RectUV(barU0, barU1, margin, margin + barThick);
        view.bar = ScreenRectTrunc(bar);
        view.slot = widgetShowing;
        auto slot = RectUV(widgetU0, widgetU1, margin, margin + barThick);
        view.slotRect = ScreenRectTrunc(slot);
        if (fluentflyout::PlaceFlyouts(view, now)) BringToFront();
    }
}

void Dock::FrameHover(const FrameState& f) {
    const auto& now = f.now; const auto& cursor = f.cursor; const auto& inMonitor = f.inMonitor; const auto& fullscreen = f.fullscreen;
    // ---- hover + preview (the tooltip: FrameTooltip)
    hover = Hit{};
    if (reveal.x > 0.9f && inMonitor && !dragging) hover = HitTestUV(cursorU, cursorV);

    // DWM previews live in a separate, non-activating owned popup. Only the
    // hovered app is passed in; app discovery and activation remain in AppModel.
    std::shared_ptr<AppItem> previewApp;
    RECT previewAnchor{};
    if (hover.kind == HitSlot && hover.slot >= 0 && !slots[hover.slot].ghost) {
        auto& s = slots[hover.slot];
        previewApp = s.app;
        float half = std::max(s.size * s.weight * 0.5f, 8 * scale);
        auto r = RectUV(s.u - half, s.u + half, 0,
            margin + pad + base * mag + 12 * scale);
        previewAnchor = ScreenRectOut(r);
    }
    windowPreview.Update(hwnd, previewApp, previewAnchor, mon, g_settings.edge, scale, cursor, now,
        reveal.x > 0.95f && reveal.target > 0.5f && !fullscreen && !menuOpen && !dragging && pressKind == HitNone);
}

void Dock::FramePanel(FrameState& f) {
    const auto& now = f.now; const auto& dt = f.dt; const auto& insideDock = f.insideDock;
    auto& anim = f.anim;
    // ---- system monitor panel: opens on hovering the sensor cell, stays while over cell/panel
    bool wantPanel = (debugForcePanel && reveal.target > 0.5f) || reveal.target > 0.5f && !menuOpen && !dragging &&
        (hover.kind == HitSensor || hover.kind == HitPanel || (panelAlpha.target > 0.5f && insideDock && NearSensorCell(cursorU)));
    if (wantPanel) { panelAlpha.target = 1; panelOutsideSince = -1; }
    else if (panelAlpha.target > 0.5f) {
        if (panelOutsideSince < 0) panelOutsideSince = now;
        if (now - panelOutsideSince > 0.25) panelAlpha.target = 0;
    }
    panelAlpha.step(dt, 320, 0.85f);
    if (!panelAlpha.settled()) anim = true;
}

// ---- tooltip
void Dock::FrameTooltip(FrameState& f) {
    const auto& now = f.now; const auto& dt = f.dt; const auto& cursor = f.cursor;
    auto& anim = f.anim;
    std::wstring tip;
    const float barTip = margin + barThick + 8 * scale - hideOff;   // tooltips of the tray controls sit just above the bar
    if (hover.kind == HitSlot && hover.slot >= 0) {
        auto& s = slots[hover.slot];
        tip = s.app ? s.app->name : L"시작";
        tooltipU = s.u;
        tooltipV = margin + pad + s.size * s.weight + s.bounceOff + 10 * scale - hideOff;
    } else if (hover.kind == HitTray) {
        tip = L"숨겨진 아이콘 표시";
        tooltipU = cells[HitTray].Mid(); tooltipV = barTip;
    } else if (hover.kind == HitClock) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t d[128] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, d, 128, nullptr);
        tip = d;
        tooltipU = cells[HitClock].Mid(); tooltipV = barTip;
    } else if (hover.kind == HitBattery) {
        tip = battery::Tooltip(g_sys.Battery());
        tooltipU = cells[HitBattery].Mid(); tooltipV = barTip;
    } else if (hover.kind == HitButton) {
        tip = (this->*Button(hover.button).tooltip)();
        tooltipU = Cell_(hover.button).Mid(); tooltipV = barTip;
    } else if (hover.kind == HitNotify && NotifyKey(hover.notify)) {
        const auto& c = notifyCells[hover.notify];
        tip = c.tip;   // empty for programs that open their own pop-up instead
        tooltipU = c.Mid(); tooltipV = barTip;
    }
    if (windowPreview.Open()) { tip.clear(); tooltipAlpha.snap(0); }
    if (!tip.empty()) { tooltipText = tip; tooltipAlpha.target = 1; }
    else tooltipAlpha.target = 0;
    tooltipAlpha.step(dt, 500, 1.0f);
    if (!tooltipAlpha.settled()) anim = true;
    NotifyHover(now, cursor);
}

void Dock::FrameCapture(const FrameState& f) {
    const auto& now = f.now; const auto& wasVisible = f.wasVisible; const auto& prevU = f.prevU; const auto& prevV = f.prevV;
    const auto& fsMonitor = f.fsMonitor; const auto& keep = f.keep; const auto& insideDock = f.insideDock; const auto& inPreview = f.inPreview;
    const auto& alwaysShown = f.alwaysShown; const auto& anim = f.anim;
    // ---- pointer reach: what the pointer changes in the picture (hover, magnification, tooltips, the
    // panel) lies around the bar. Moves anywhere else need no redraw - an always-shown dock would
    // otherwise redraw on every move on any monitor, a game's mouse movement included.
    const float reach = margin + barThick + base * (mag - 1) + 32 * scale, side = 32 * scale;
    auto nearBar = [&](float u, float v) { return v < reach && u > barU0 - side && u < barU1 + side; };
    const bool panelShown = PanelShown();
    const bool pointerNear = nearBar(cursorU, cursorV) || insideDock || panelShown || dragging;

    // ---- capture lifetime
    bool visibleNow = RevealShown();
    // An always-shown dock nobody is using rests: no vsync-paced frames, its glass follows the screen at
    // the main loop's idle pace (~30 fps). While a fullscreen app (a game) is in front on another
    // monitor it also stops capturing and keeps its last glass: only the clock and sensor text update.
    bool engaged = keep || pointerNear || inPreview || windowPreview.Open() ||
                   tooltipAlpha.target > 0.5f || magAmount.target > 0.5f;
    bool resting = alwaysShown && visibleNow && !engaged && !rec.recording;
    bool frozen = resting && fsMonitor && fsMonitor != monitor;
    if ((visibleNow && !frozen) || rec.recording) lastCaptureWanted = now;
    if (now - lastCaptureWanted < 0.05) {
        // a failing start (RDP, protected display...) is retried every few seconds, not every frame
        if (!capture.Active() && now >= nextCaptureTry && !capture.Start(monitor)) nextCaptureTry = now + 3.0;
    }
    else if (capture.Active() && now - lastCaptureWanted > 6.0) capture.Stop();
    if (!visibleNow) glassKept = false;   // hidden: the next reveal waits for a fresh frame, as before
    // frozen: no frames are read (no copies, no redraws); the capture itself ends 6 s later.
    // resting: about 30 frames a second, however often the main loop comes by
    bool pollNow = !frozen && (!resting || now >= nextRestingPoll);
    if (capture.Active() && pollNow) {
        if (resting) nextRestingPoll = now + 0.028;
        if (capture.Poll(backdrop.tex.Get(), winX - mon.left, winY - mon.top, winW, winH, rec.recording ? rec.fullTex.tex.Get() : nullptr)) {
            backdropDirty = true;
            if (visibleNow) { dirty = true; glassKept = true; }
        }
    }
    liveGlass = resting && !frozen && capture.Active();

    bool peeking = sbPeek.x > 0.001f || sbPeek.target > 0;
    for (auto& s : slots) if (s.peek.x > 0.001f || s.peek.target > 0) peeking = true;
    if (anim) dirty = true;
    if (visibleNow && (pointerNear || nearBar(prevU, prevV)) && (prevU != cursorU || prevV != cursorV)) dirty = true;
    if (wasVisible != (reveal.x > 0.002f)) dirty = true;
    busy = anim || peeking || rec.recording || (visibleNow && !resting);
    restingPrev = resting;
}

float Dock::UnpinLine() const {
    const float q = 32 * scale;   // UpdateRegion's rounding
    const float regionTop = std::min(std::ceil(IconStripTop() / q) * q, T);
    return std::max(regionTop + base / 2, MagnifiedTop());
}

void Dock::UpdateRegion() {
    RECT r = { 0, 0, 0, 0 };
    auto add = [&](D2D1_RECT_F f) {
        RECT q = { (LONG)std::floor(f.left), (LONG)std::floor(f.top), (LONG)std::ceil(f.right), (LONG)std::ceil(f.bottom) };
        if (IsRectEmpty(&r)) r = q; else UnionRect(&r, &r, &q);
    };
    if (RevealShown()) {
        float q = 32 * scale;
        float u0 = std::floor((barU0 - 14 * scale) / q) * q, u1 = std::ceil((barU1 + 14 * scale) / q) * q;
        float vTop = IconStripTop();
        if (magAmount.x > 0.01f || magAmount.target > 0.5f) vTop = MagnifiedTop();
        for (auto& s : slots) if (s.bounceOff > 0 || s.bounceCount > 0 || s.bounceLoop) vTop = std::max(vTop, margin + pad + s.size + base * 0.95f + 4 * scale);
        if (sbCount > 0) vTop = std::max(vTop, margin + barThick + base * 0.95f);
        vTop = std::ceil(vTop / q) * q;
        add(RectUV(u0, u1, 0, std::min(vTop, T)));
        if (tooltipAlpha.x > 0.01f || tooltipAlpha.target > 0.5f) {
            float w = TextWidth(g_gfx.fmtTooltip.Get(), tooltipText) + 28 * scale;
            float ext = IsVertical(g_settings.edge) ? w : 34 * scale;
            float across = IsVertical(g_settings.edge) ? 34 * scale : w;
            add(RectUV(tooltipU - across / 2 - 4, tooltipU + across / 2 + 4, tooltipV - 4, std::min(tooltipV + ext + 4, T)));
        }
    }
    for (auto& s : slots) {
        if (s.peek.x > 0.001f || s.peek.target > 0) {
            float sz = base * 1.0f;
            add(RectUV(s.u - sz / 2 - 6 * scale, s.u + sz / 2 + 6 * scale, 0, margin + pad + sz + base * 0.95f + 8 * scale));
        }
    }
    if (PanelShown()) {
        D2D1_RECT_F pr = panelRect;
        pr.left -= 20 * scale; pr.top -= 20 * scale; pr.right += 20 * scale; pr.bottom += 24 * scale;
        add(pr);
    }
    if (sbPeek.x > 0.001f || sbPeek.target > 0) {
        bool vert = IsVertical(g_settings.edge);
        float c = cells[HitSensor].Mid(), a = vert ? 52 * scale : 112 * scale, depth = vert ? 84 * scale : 40 * scale;
        add(RectUV(c - a / 2 - 6 * scale, c + a / 2 + 6 * scale, 0, margin + depth + base * 0.95f + 10 * scale));
    }
    RECT win = { 0, 0, winW, winH };
    IntersectRect(&r, &r, &win);
    if (!EqualRect(&r, &curRegion)) {
        curRegion = r;
        HRGN rg = CreateRectRgn(r.left, r.top, r.right, r.bottom);
        if (!SetWindowRgn(hwnd, rg, FALSE)) DeleteObject(rg);   // the window owns it on success
    }
    // Win+Shift+S's window mode picks windows by their rectangle, not their region: with nothing of the dock
    // up, its whole strip (591 px thick at 150 %) was offered over the window behind it and snipped as that
    // window's bottom (10-28). While the region is empty the window is cloaked - kept, as a window
    // on another virtual desktop, but not one on screen. One DWM call when the dock comes up or goes.
    BOOL hide = IsRectEmpty(&r);
    if (hide != cloaked && SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &hide, sizeof(hide)))) cloaked = hide;
}
