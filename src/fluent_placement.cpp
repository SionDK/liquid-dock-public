#include "fluent_internal.h"

namespace fluentflyout {

// ---------------------------------------------------------------- flyouts
// FluentFlyout places its flyouts in the work area (a hidden dock reserves none) and animates them
// in. Next to a shown dock they move clear of the glass once that animation is over: along the dock
// to the widget at once (FluentFlyout keeps that while it animates), off the edge after it.
namespace {

struct Flyout {
    HWND h = nullptr;
    enum State { Waiting, Moving, Placed, Left } state = Waiting;
    double shownAt = 0, movedAt = 0, settledAt = 0;
    bool aligned = false, raised = false, nearShownDock = false;
    RECT from{}, to{}, expect{};
    double moveStart = 0;
};

std::vector<Flyout> g_flyouts;
std::unordered_map<HMONITOR, int> g_lift;   // per monitor: the move off the edge given to the flyouts there
HWINEVENTHOOK g_hooks[2] = {};

double AnimSeconds() {
    static const double kSeconds[] = { 0, 0.15, 0.3, 0.45, 0.6, 0.9 };
    return kSeconds[std::clamp(g_ff.animationSpeed, 0, 5)];
}

// FluentFlyout's flyouts: its top-level windows of flyout size, not layered (its menus and tooltips are).
bool IsFlyoutWindow(HWND h) {
    if (!IsWindowVisible(h) || GetAncestor(h, GA_PARENT) != GetDesktopWindow()) return false;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) return false;
    wchar_t title[32] = {};
    GetWindowTextW(h, title, 32);
    if (!wcscmp(title, L"TaskbarWindow")) return false;
    RECT r{};
    if (!GetWindowRect(h, &r) || r.right <= r.left || r.bottom <= r.top) return false;
    UINT dpi = GetDpiForWindow(h);
    float s = dpi ? dpi / 96.0f : 1.0f;
    return r.right - r.left <= 520 * s && r.bottom - r.top <= 360 * s;
}

void MoveFlyout(Flyout& f, int x, int y) {
    RECT r{};
    GetWindowRect(f.h, &r);
    f.expect = { x, y, x + (r.right - r.left), y + (r.bottom - r.top) };
    SetWindowPos(f.h, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    auto it = std::find_if(g_flyouts.begin(), g_flyouts.end(), [&](const Flyout& f) { return f.h == h; });
    if (ev == EVENT_OBJECT_HIDE || ev == EVENT_OBJECT_DESTROY) {
        if (it != g_flyouts.end()) g_flyouts.erase(it);
        return;
    }
    double now = NowSeconds();
    if (ev == EVENT_OBJECT_SHOW) {
        if (!IsFlyoutWindow(h)) return;
        Flyout f;
        f.h = h;
        f.shownAt = f.movedAt = now;
        if (it != g_flyouts.end()) *it = f; else g_flyouts.push_back(f);
        return;
    }
    if (ev != EVENT_OBJECT_LOCATIONCHANGE || it == g_flyouts.end() || it->state == Flyout::Moving) return;
    RECT r{};
    GetWindowRect(h, &r);
    if (EqualRect(&r, &it->expect)) return;   // its own move
    if (it->state == Flyout::Placed && now - it->settledAt < 0.2) return;   // the last of its own steps
    it->movedAt = now;
    if (it->state == Flyout::Placed) it->state = Flyout::Left;   // FluentFlyout moves it again (closing it): its business
}

}   // namespace

void Unhook() {
    for (auto& h : g_hooks)
        if (h) { UnhookWinEvent(h); h = nullptr; }
}

void Hook() {
    Unhook();
    if (!g_pid) return;
    g_hooks[0] = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, OnWinEvent, g_pid, 0, WINEVENT_OUTOFCONTEXT);
    g_hooks[1] = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnWinEvent, g_pid, 0, WINEVENT_OUTOFCONTEXT);
}

// FluentFlyout gone (or another one found): its flyouts and their stacks forgotten.
void ForgetFlyouts() {
    g_flyouts.clear();
    g_lift.clear();
}

// Every Tick: the flyouts closed or hidden meanwhile are dropped.
void PruneFlyouts() {
    for (size_t i = g_flyouts.size(); i-- > 0;)
        if (!IsWindow(g_flyouts[i].h) || !IsWindowVisible(g_flyouts[i].h)) g_flyouts.erase(g_flyouts.begin() + i);
}

namespace {

// Centered on the widget's slot, kept on the monitor.
int AlignedLeft(const RECT& r, const DockView& v, const RECT& mon) {
    int w = r.right - r.left, margin = (int)std::lround(16 * v.scale);
    int x = (v.slotRect.left + v.slotRect.right) / 2 - w / 2;
    return std::clamp(x, (int)mon.left + margin, std::max((int)mon.left + margin, (int)mon.right - margin - w));
}

}   // namespace

bool PlaceFlyouts(const DockView& v, double now) {
    if (g_flyouts.empty()) g_lift.clear();   // the next flyout starts afresh
    if (g_flyouts.empty() || !g_pid || IsVertical(v.edge)) return false;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(v.monitor, &mi)) return false;
    const RECT& mon = mi.rcMonitor;
    const bool bottom = v.edge == Edge::Bottom;
    const int gap = (int)std::lround(12 * v.scale);
    int& lift = g_lift[v.monitor];
    bool raise = false, anyPlaced = false;
    for (auto& f : g_flyouts) {
        if (f.state == Flyout::Left || MonitorFromWindow(f.h, MONITOR_DEFAULTTONEAREST) != v.monitor) continue;
        RECT cur{};
        if (!GetWindowRect(f.h, &cur)) continue;
        if (f.state == Flyout::Placed) { anyPlaced = true; continue; }
        if (f.state == Flyout::Waiting) {
            f.nearShownDock = v.shown;
            if (!v.shown) continue;   // a hidden dock: FluentFlyout's own place
            int cy = (cur.top + cur.bottom) / 2, my = (mon.top + mon.bottom) / 2;
            if (bottom ? cy < my : cy > my) { f.state = Flyout::Left; continue; }   // set to the far side of the screen
            bool across = bottom ? cur.bottom > v.bar.top - gap : cur.top < v.bar.bottom + gap;
            if (across && !f.raised) { f.raised = true; raise = true; }
            if (!f.aligned) {
                f.aligned = true;
                if (v.slot) {
                    int x = AlignedLeft(cur, v, mon);
                    if (x != cur.left) MoveFlyout(f, x, cur.top);
                }
            }
            if (now - f.shownAt < AnimSeconds() + 0.03 || now - f.movedAt < 0.06) continue;
            RECT to = cur;
            if (v.slot) OffsetRect(&to, AlignedLeft(cur, v, mon) - cur.left, 0);
            int need = bottom ? std::min(0, (int)(v.bar.top - gap - cur.bottom)) : std::max(0, (int)(v.bar.bottom + gap - cur.top));
            int dy = bottom ? std::min(need, lift) : std::max(need, lift);   // a stack moves together
            OffsetRect(&to, 0, dy);
            if (to.top < mon.top) OffsetRect(&to, 0, mon.top - to.top);
            if (to.bottom > mon.bottom) OffsetRect(&to, 0, mon.bottom - to.bottom);
            lift = dy;
            if (EqualRect(&to, &cur)) { f.state = Flyout::Placed; f.settledAt = now; anyPlaced = true; continue; }
            f.from = cur; f.to = to; f.moveStart = now;
            f.state = Flyout::Moving;
        }
        if (f.state == Flyout::Moving) {
            anyPlaced = true;
            float t = clampf(float((now - f.moveStart) / 0.22), 0, 1);
            float e = 1 - (1 - t) * (1 - t) * (1 - t);
            MoveFlyout(f, (int)std::lround(lerpf((float)f.from.left, (float)f.to.left, e)),
                          (int)std::lround(lerpf((float)f.from.top, (float)f.to.top, e)));
            if (t >= 1) {
                f.state = Flyout::Placed;
                f.settledAt = now;
                // clear of the glass: on top of the dock again
                SetWindowPos(f.h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            }
        }
    }
    if (!anyPlaced) lift = 0;
    return raise;
}

bool FlyoutAt(HMONITOR monitor, POINT p) {
    for (auto& f : g_flyouts) {
        if (f.state != Flyout::Placed && f.state != Flyout::Moving) continue;
        RECT r{};
        if (GetWindowRect(f.h, &r) && PtInRect(&r, p) && MonitorFromWindow(f.h, MONITOR_DEFAULTTONEAREST) == monitor) return true;
    }
    return false;
}

bool Busy() {
    for (auto& f : g_flyouts)
        if (f.state == Flyout::Moving || (f.state == Flyout::Waiting && f.nearShownDock)) return true;
    return false;
}

}   // namespace fluentflyout
