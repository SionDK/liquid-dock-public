#include "tray_host_internal.h"

namespace trayhost {

// ---------------------------------------------------------------- hosting (a program's taskbar widget)
// A program that puts a widget on the taskbar (FluentFlyout) makes it a child window of the first
// "Shell_TrayWnd" - this window while the host runs. Hosting on, the dock shows this window where it
// keeps a slot for the widget: transparent, cut to the widget's own region, so it takes no clicks
// beside it. The program lays its widget out in this window as it would in the taskbar.
namespace {
std::atomic<bool> g_hosting{ false };
std::atomic<HWND> g_hostOnly{ nullptr };  // the one hosted window shown (others that moved in stay cut away)
struct Placement { bool valid = false, shown = false, raise = false; RECT rect{}, clip{}; HWND over = nullptr; };
std::mutex g_placeMu;
Placement g_place;                        // the dock's request (UI thread writes, host thread applies)
}   // namespace
std::mutex g_hostedMu;
RECT g_hostedBox{};                       // what the hosted windows show, in this window's coordinates
bool g_hostedFitted = false;              // the hosted window spans this window: laid out for its size
HWND g_hostedChild = nullptr;
HostedWidget g_hostedWidget;              // host thread (tray_host_internal.h)
struct Realigner {                        // the realign thread's own (each such thread has one)
    HANDLE wake = nullptr;
    std::atomic<HWND> to{ nullptr };      // the widget to put back...
    std::atomic<HWND> host{ nullptr };    // ...in this window
    std::atomic<DWORD> hostThread{ 0 };   // the host thread, woken once it is back
    std::atomic<bool> stop{ false };
};

namespace {

// The window programs find as "the taskbar's" tray area (FluentFlyout aligns a widget set to the
// right end with it): all of this window, or its right end while hosting.
void PlaceNotifyWnd() {
    if (!g_notifyWnd) return;
    RECT r{};
    GetClientRect(g_wnd, &r);
    if (g_hostedWidget.hostingApplied) SetWindowPos(g_notifyWnd, nullptr, std::max(0L, r.right - 1), 0, 1, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    else SetWindowPos(g_notifyWnd, nullptr, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
}

}   // namespace

namespace {

// The shown widget (FluentFlyout's) sits at this window's corner: FluentFlyout's timer puts it at the
// client point of this window's top-left, as big as this window (TaskbarWindow.CalculateAndSetPosition).
// It drifts off when this window moves while it draws - and the dock moves it on every frame of the
// magnification. WPF draws it as a layered child window: each frame its render thread reads where the
// window is on the screen (ClientToScreen), draws, and shows the frame with UpdateLayeredWindowIndirect
// at that place, turned into its parent's coordinates then - which moves the window there. Should this
// window move between the read and the showing, the widget lands where this window was, and every later
// frame reads that place again and keeps it (2026-11-13, while magnifying: off its place 81% of the time,
// up to 3 s at a time). Left alone until FluentFlyout's next tick (1.5 s, which may lose the same way),
// the widget was cut by this window's edge - the dock even moved this window by what the widget showed,
// so further off with each move it chased. Put back at once (Realign), asynchronously as FluentFlyout does.
//
// The move is asked for on a thread of its own that has no window. The widget is a child of this
// window, so Windows joins this thread's input to FluentFlyout's thread: SWP_ASYNCWINDOWPOS then does
// not post the move but makes it right here, waiting for FluentFlyout's thread - busy for a moment
// (layout, its UIA lookups) or hung, and meanwhile the host would not follow the dock or stop.
// A thread with no window shares no input with it and returns at once (2026-11-13, hidden windows of two
// processes, the widget's thread busy for 700 ms: 0 ms there, 660 ms here).

// Where the widget is: off its place in the host window, in place, or not in the host window any more.
enum class Spot { Off, InPlace, Gone };
Spot SpotOf(Realigner* r, HWND c) {
    HWND parent = GetAncestor(c, GA_PARENT);
    RECT pr{}, cr{};
    if (!parent || parent != r->host || !GetWindowRect(parent, &pr) || !GetWindowRect(c, &cr)) return Spot::Gone;
    return cr.left != pr.left || cr.top != pr.top ? Spot::Off : Spot::InPlace;
}

// Put back. The same race may undo it: a frame read before the move and shown after it puts the widget
// back where it was (2026-11-13, measured: off again 2 ms later, time after time, while the visualizer
// drew). So looked at again 10 to 25 ms later - other moments of WPF's frames - and put back again, up to
// 6 more times (7 moves), and looked at once more after the last; the host thread asks anew should it
// still be off then. Moved only while it is off and still in the host window. Once it is back, the host
// thread is woken: the host window's region followed the widget where it had drifted, and would cut it
// until the next check (0.1 s).
void PutBackNow(Realigner* r, HWND c) {
    static constexpr DWORD kWaits[] = { 10, 13, 16, 19, 22, 25, 25 };   // after each move
    for (size_t i = 0;; ++i) {
        const Spot spot = SpotOf(r, c);
        if (spot != Spot::Off) {
            if (spot == Spot::InPlace) PostThreadMessageW(r->hostThread, kPlaceMsg, kPlaceHosting, 0);
            return;
        }
        if (i == std::size(kWaits)) return;
        SetWindowPos(c, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
        if (WaitForSingleObject(r->wake, kWaits[i]) == WAIT_OBJECT_0) {   // asked again (or to stop) meanwhile
            SetEvent(r->wake);
            return;
        }
    }
}

DWORD WINAPI RealignThread(void* p) {
    auto* r = (Realigner*)p;
    while (WaitForSingleObject(r->wake, INFINITE) == WAIT_OBJECT_0 && !r->stop) {
        HWND c = r->to.exchange(nullptr);
        if (c) PutBackNow(r, c);
    }
    return 0;
}

void PutBack(HWND c) {
    if (!g_hostedWidget.realigner) {
        auto* r = new Realigner;
        r->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = r->wake ? CreateThread(nullptr, 0, RealignThread, r, 0, nullptr) : nullptr;
        if (!t) {
            if (r->wake) CloseHandle(r->wake);
            delete r;
            return;
        }
        g_hostedWidget.realigner = r;
        g_hostedWidget.realignThread = t;
    }
    g_hostedWidget.realigner->host = g_wnd;
    g_hostedWidget.realigner->hostThread = GetCurrentThreadId();   // PutBack runs on the host thread
    g_hostedWidget.realigner->to = c;
    SetEvent(g_hostedWidget.realigner->wake);
}

// Asked for when the widget is found off its place, again at once when it is off by another amount
// (it drifted further meanwhile) or after it was seen back in place, else at most every 0.1 s while it
// stays off. Returns whether it was off.
bool Realign(HWND c, int ox, int oy) {
    if (!ox && !oy) {
        if (c == g_hostedWidget.realignOf) g_hostedWidget.realignAt = -1;   // back: the next drift is put back at once, even by the same amount
        return false;
    }
    const double now = NowSeconds();
    if (c != g_hostedWidget.realignOf || ox != g_hostedWidget.realignOff.x || oy != g_hostedWidget.realignOff.y || now - g_hostedWidget.realignAt >= 0.1) {
        g_hostedWidget.realignOf = c;
        g_hostedWidget.realignOff = { ox, oy };
        g_hostedWidget.realignAt = now;
        PutBack(c);
        ++g_hostedWidget.realigned;
        if (g_hostedWidget.realigned <= 3 || g_hostedWidget.realigned % 100 == 0)
            Log(L"tray host: the widget was %d,%d off this window's corner (moved while it drew) - put back (%d time%s)",
                ox, oy, g_hostedWidget.realigned, g_hostedWidget.realigned == 1 ? L"" : L"s");
    }
    return true;
}

// The shown widget's region. FluentFlyout sets it again on every tick, and now and then it reads as none
// for a moment: as the whole window the widget seemed 480 px wide, and the dock moved its slot for that
// moment. The region last read stands for a second; a widget without one is all window.
void WidgetRegion(HWND c, HRGN r, int cw, int ch) {
    const double now = NowSeconds();
    if (GetWindowRgn(c, r) != ERROR) {
        if (!g_hostedWidget.widgetRgn) g_hostedWidget.widgetRgn = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(g_hostedWidget.widgetRgn, r, nullptr, RGN_COPY);
        g_hostedWidget.widgetRgnOf = c;
        g_hostedWidget.widgetRgnAt = now;
    } else if (c == g_hostedWidget.widgetRgnOf && g_hostedWidget.widgetRgn && now - g_hostedWidget.widgetRgnAt < 1.0) {
        CombineRgn(r, g_hostedWidget.widgetRgn, nullptr, RGN_COPY);
    } else {
        SetRectRgn(r, 0, 0, cw, ch);
    }
}

}   // namespace

// The realign thread ends - before the widget goes back to Explorer's taskbar, where it is not to be
// moved. It only posts; should it be held anyway, it is left with what it uses (its own stop flag and
// event, so a host started again makes a thread of its own and this one still ends once free).
void StopRealign() {
    if (g_hostedWidget.realigner) {
        g_hostedWidget.realigner->stop = true;
        SetEvent(g_hostedWidget.realigner->wake);
        if (WaitForSingleObject(g_hostedWidget.realignThread, 500) == WAIT_OBJECT_0) {
            CloseHandle(g_hostedWidget.realigner->wake);
            delete g_hostedWidget.realigner;
        }
        CloseHandle(g_hostedWidget.realignThread);
        g_hostedWidget.realigner = nullptr;
        g_hostedWidget.realignThread = nullptr;
    }
}

namespace {

// The host thread ends: what it kept of the widget goes.
void ForgetWidget() {
    StopRealign();
    if (g_hostedWidget.widgetRgn) { DeleteObject(g_hostedWidget.widgetRgn); g_hostedWidget.widgetRgn = nullptr; }
    g_hostedWidget.widgetRgnOf = g_hostedWidget.realignOf = nullptr;
    g_hostedWidget.realignOff = {};
    g_hostedWidget.realigned = 0;
    g_hostedWidget.widgetRgnAt = g_hostedWidget.realignAt = -1;
}

}   // namespace

// The host thread ends, its window destroyed: hosting off, and what it kept of the hosting goes - the
// region's copy, the widget's region and realigning, what it published for the dock (HostedState).
void EndHosting() {
    g_hostedWidget.hostingApplied = false;
    g_hostedWidget.regionShown = false;
    if (g_hostedWidget.appliedRgn) { DeleteObject(g_hostedWidget.appliedRgn); g_hostedWidget.appliedRgn = nullptr; }
    ForgetWidget();
    {
        std::lock_guard<std::mutex> lk(g_hostedMu);
        g_hostedBox = {}; g_hostedChild = nullptr; g_hostedFitted = false;
    }
}

// What the hosted windows show: their regions (the whole window without one), clipped to them.
// Published for the dock. While hosting it is also this window's region - an empty one while the
// dock keeps the widget away: the window itself stays shown, so the hosted program never sees its
// widget's parent hidden (as the dock leaves Explorer's taskbar shown, cut to nothing). at: where the
// window is about to go (the cut at the monitor's edge depends on it), if not where it is.
// The shown widget counts as at this window's corner for the dock (Realign puts it back there).
void UpdateHosted(const RECT* at) {
    HRGN all = CreateRectRgn(0, 0, 0, 0);
    RECT host{};
    GetWindowRect(g_wnd, &host);
    const RECT dest = at ? *at : host;
    HWND first = nullptr;
    bool fitted = false;
    RECT box{};
    const HWND only = g_hostOnly;
    for (HWND c = GetWindow(g_wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (c == g_notifyWnd || !(GetWindowLongPtrW(c, GWL_STYLE) & WS_VISIBLE)) continue;
        if (only && c != only) continue;   // another program's taskbar widget that moved in: not shown
        RECT cr{};
        GetWindowRect(c, &cr);
        int cw = cr.right - cr.left, ch = cr.bottom - cr.top, ox = cr.left - host.left, oy = cr.top - host.top;
        const bool widget = only && c == only && g_hostedWidget.hostingApplied;
        const bool off = widget && Realign(c, ox, oy);
        HRGN r = CreateRectRgn(0, 0, 0, 0);
        if (widget) WidgetRegion(c, r, cw, ch);
        else if (GetWindowRgn(c, r) == ERROR) SetRectRgn(r, 0, 0, cw, ch);
        HRGN bounds = CreateRectRgn(0, 0, cw, ch);
        CombineRgn(r, r, bounds, RGN_AND);
        DeleteObject(bounds);
        if (off) {   // the dock lays the slot out for the widget where it goes back to, not where it drifted
            RECT b{};
            if (GetRgnBox(r, &b) != NULLREGION) UnionRect(&box, &box, &b);
        }
        OffsetRgn(r, ox, oy);   // this window shows it where it is meanwhile
        if (!off) {
            RECT b{};
            if (GetRgnBox(r, &b) != NULLREGION) UnionRect(&box, &box, &b);
        }
        CombineRgn(all, all, r, RGN_OR);
        DeleteObject(r);
        if (!first) {
            first = c;
            fitted = (off || (ox == 0 && oy == 0)) && cw == host.right - host.left && ch == host.bottom - host.top;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_hostedMu);
        g_hostedBox = box; g_hostedChild = first; g_hostedFitted = fitted;
    }
    if (!g_hostedWidget.hostingApplied) { DeleteObject(all); return; }
    RECT clip{};
    {
        std::lock_guard<std::mutex> lk(g_placeMu);
        clip = g_place.clip;
    }
    if (!g_hostedWidget.regionShown) SetRectRgn(all, 0, 0, 0, 0);
    else if (!IsRectEmpty(&clip)) {   // sliding in with the dock: cut at the edge of its monitor, as the dock is
        HRGN c = CreateRectRgn(clip.left - dest.left, clip.top - dest.top, clip.right - dest.left, clip.bottom - dest.top);
        CombineRgn(all, all, c, RGN_AND);
        DeleteObject(c);
    }
    if (g_hostedWidget.appliedRgn && EqualRgn(all, g_hostedWidget.appliedRgn)) { DeleteObject(all); return; }
    if (!g_hostedWidget.appliedRgn) g_hostedWidget.appliedRgn = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(g_hostedWidget.appliedRgn, all, nullptr, RGN_COPY);
    if (!SetWindowRgn(g_wnd, all, TRUE)) DeleteObject(all);   // the window owns it on success
}

namespace {

// Whether `over` (the dock's window) is above this one.
bool Covered(HWND over) {
    if (!over || !IsWindow(over)) return false;
    int guard = 0;
    for (HWND w = GetWindow(g_wnd, GW_HWNDPREV); w && guard < 1024; w = GetWindow(w, GW_HWNDPREV), ++guard)
        if (w == over) return true;
    return false;
}

// The widget sits on the dock's glass. The dock comes to the front on its own at times (shown again,
// next to a shell pop-up), and should it come over the widget, the widget goes back on top.
void KeepAbove(HWND over) {
    if (!g_hostedWidget.regionShown || !Covered(over)) return;
    SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (g_hostedWidget.coveredLogged) return;
    g_hostedWidget.coveredLogged = true;
    Log(L"tray host: the dock had come over the widget - back on top");
}

// The log describes a hosted widget once it is seen: where, how big, how it draws, whether it is on top.
void DescribeShown(HWND over) {
    HWND child = nullptr;
    RECT box{};
    {
        std::lock_guard<std::mutex> lk(g_hostedMu);
        child = g_hostedChild; box = g_hostedBox;
    }
    if (!child || child == g_hostedWidget.shownLogged) return;
    g_hostedWidget.shownLogged = child;
    RECT host{}, cr{}, cut{};
    GetWindowRect(g_wnd, &host);
    GetWindowRect(child, &cr);
    if (g_hostedWidget.appliedRgn) GetRgnBox(g_hostedWidget.appliedRgn, &cut);
    const bool layered = (GetWindowLongPtrW(child, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
    Log(L"tray host: widget seen - window %ld,%ld %ldx%ld, its window %ld,%ld %ldx%ld drawn %s, widget %ld,%ld-%ld,%ld, cut to %ld,%ld-%ld,%ld, %s",
        host.left, host.top, host.right - host.left, host.bottom - host.top,
        cr.left - host.left, cr.top - host.top, cr.right - cr.left, cr.bottom - cr.top, layered ? L"as a layered window" : L"into this window",
        box.left, box.top, box.right, box.bottom, cut.left, cut.top, cut.right, cut.bottom,
        !over ? L"no dock given" : Covered(over) ? L"UNDER the dock" : L"above the dock");
}

}   // namespace

// The dock's latest request: where the window is and whether the widget shows. While hosting the
// window is always a shown window; whether anything of it is seen is its region's business.
void ApplyPlacement() {
    Placement p;
    {
        std::lock_guard<std::mutex> lk(g_placeMu);
        p = g_place;
        g_place.raise = false;
    }
    if (!g_hostedWidget.hostingApplied) return;
    const bool shown = p.valid && p.shown && !IsRectEmpty(&p.rect);
    const bool wasShown = g_hostedWidget.regionShown;
    if (wasShown && !shown) { g_hostedWidget.regionShown = false; UpdateHosted(); }   // gone at once, then moved
    RECT cur{};
    GetWindowRect(g_wnd, &cur);
    const bool moved = p.valid && !IsRectEmpty(&p.rect) && !EqualRect(&cur, &p.rect);
    const bool sized = moved && (cur.right - cur.left != p.rect.right - p.rect.left || cur.bottom - cur.top != p.rect.bottom - p.rect.top);
    const bool visible = IsWindowVisible(g_wnd) != FALSE;
    const bool appearing = shown && !wasShown;
    if (wasShown && shown && moved) UpdateHosted(&p.rect);   // the cut goes ahead of the move (no sliver on the next monitor)
    UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (!moved) flags |= SWP_NOMOVE | SWP_NOSIZE;
    if (!visible) flags |= SWP_SHOWWINDOW;   // also undoes "hide the taskbar" tools
    if (!p.raise && !appearing) flags |= SWP_NOZORDER;   // coming into view: on top of the dock
    if (moved || !(flags & SWP_NOZORDER) || (flags & SWP_SHOWWINDOW)) {
        RECT r = moved ? p.rect : cur;
        g_hostedWidget.showing = true;
        SetWindowPos(g_wnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, flags);
        g_hostedWidget.showing = false;
    }
    if (sized) PlaceNotifyWnd();
    if (appearing) { g_hostedWidget.regionShown = true; UpdateHosted(); }   // in place first, then seen
    else if (sized) UpdateHosted();
    if (g_hostedWidget.regionShown) {
        KeepAbove(p.over);
        DescribeShown(p.over);
    }
}

// Hosting switched: the window goes where the dock wants it, or back to mirroring Explorer's taskbar.
void ApplyHosting() {
    bool on = g_hosting;
    if (on == g_hostedWidget.hostingApplied) return;
    g_hostedWidget.hostingApplied = on;
    g_hostedWidget.regionShown = false;
    g_hostedWidget.coveredLogged = false;
    if (on) {
        UpdateHosted();      // an empty region before the window shows
        ApplyPlacement();    // shows it (cut to nothing until the dock wants the widget seen)
        PlaceNotifyWnd();
        UpdateHosted();
        return;
    }
    g_hostedWidget.showing = false;
    SetWindowPos(g_wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    SetWindowRgn(g_wnd, nullptr, FALSE);
    if (g_hostedWidget.appliedRgn) { DeleteObject(g_hostedWidget.appliedRgn); g_hostedWidget.appliedRgn = nullptr; }
    MirrorRect();
    PlaceNotifyWnd();
    UpdateHosted();
}

// Programs that look at "the taskbar's" rectangle see Explorer's.
void MirrorRect() {
    HWND ex = ExplorerTray();
    RECT er{}, mine{};
    if (!ex || !GetWindowRect(ex, &er) || !GetWindowRect(g_wnd, &mine) || EqualRect(&er, &mine)) return;
    SetWindowPos(g_wnd, nullptr, er.left, er.top, er.right - er.left, er.bottom - er.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    PlaceNotifyWnd();
}

void SetHosting(bool on, HWND only) {
    // called on every pass of the main loop: the host thread hears only of changes (a new widget window
    // to show, hosting switched), and then at once instead of at its next check
    const bool onlyChanged = g_hostOnly.exchange(only) != only;
    const bool onChanged = g_hosting.exchange(on) != on;
    if ((onlyChanged || onChanged) && g_thread && g_ready) PostThreadMessageW(g_threadId, kPlaceMsg, kPlaceHosting, 0);
}

void Place(bool shown, const RECT& rect, bool raise, const RECT& clip, HWND over) {
    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_placeMu);
        changed = !g_place.valid || g_place.shown != shown || !EqualRect(&g_place.rect, &rect) || !EqualRect(&g_place.clip, &clip) ||
                  (raise && !g_place.raise) || g_place.over != over;
        g_place.valid = true;
        g_place.shown = shown;
        g_place.rect = rect;
        g_place.clip = clip;
        g_place.raise = g_place.raise || raise;
        g_place.over = over;
    }
    if (changed && g_thread && g_ready) PostThreadMessageW(g_threadId, kPlaceMsg, kPlaceHosting, 0);
}

Hosted HostedState() {
    std::lock_guard<std::mutex> lk(g_hostedMu);
    Hosted h;
    h.window = g_hostedChild;
    h.box = g_hostedBox;
    h.fitted = g_hostedFitted;
    return h;
}

}   // namespace trayhost
