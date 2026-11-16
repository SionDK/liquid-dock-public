// The window preview's opening rules as the dock drives them, its layout next to icons at the corners
// of monitors (100 %, 125 %, a portrait monitor at negative coordinates), two docks' previews, clicks
// on a card that is closing, and its DWM relationships under long random use - with an in-memory
// window/DWM model, as tests/window_preview_test.cpp; the wheel turning pages, a notch at a time; and what
// it asks of the window manager and DWM, counted: a hidden dock's passes, an icon moving under the open
// popup (100 % and 150 %), a popup at rest. No real windows, input, Explorer or other programs are involved. One PASS/FAIL line per check; the exit code is the number of failures.
#include "../src/common.h"
#include "../src/apps.h"
#include <dwmapi.h>
#include <map>
#include <set>
#include <random>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include "portable_check.h"
#define private public
#include "../src/window_preview.h"
#undef private

struct Source {
    DWORD pid = 3; bool alive = true, minimized = false, protectedContent = false;
    SIZE dwm{1920, 1080};       // what DWM reports for its thumbnail; while minimized, the image it kept
    bool dwmFails = false, updateFails = false;
    RECT normalRect{100, 100, 1300, 800};
    RECT minimizedRect{-32000, -32000, -31840, -31972};   // a minimized window itself: its caption, off screen
};
static std::map<HWND, Source> sources;
static std::map<HTHUMBNAIL, HWND> live;          // DWM relationships held now -> their source window
static std::map<HTHUMBNAIL, HWND> liveDest;      // ...and the window they draw into
static std::map<HTHUMBNAIL, DWM_THUMBNAIL_PROPERTIES> lastProps;
static std::map<HWND, bool> popups;              // the previews' own popups: shown or not
static INT_PTR nextThumbnail = 0x700000;
static int registrations = 0, strayUnregisters = 0, strayUpdates = 0, activations = 0, sourceTouched = 0;
static bool failRegister = false;
static HWND front = (HWND)0x9090;
static WindowPreview* current = nullptr;
static HWND captured = nullptr;
static std::vector<HWND> asked;                  // the windows the preview asked the AppModel to close
static bool dismissWhileAsking = false;          // the AppModel's close waits on a question meanwhile the popup goes
// what the preview asks of the window manager and DWM, counted (tests/preview_rules_test.cpp's cost checks)
static int popupShows = 0, popupMoves = 0, regions = 0, invalidates = 0, sizeAsks = 0, affinityAsks = 0, propUpdates = 0;
static std::map<HWND, RECT> popupAt;             // where SetWindowPos last put each popup

static bool Known(HWND h) { return sources.find(h) != sources.end(); }
static BOOL WINAPI FakeIsWindow(HWND h) { auto it = sources.find(h); return it != sources.end() && it->second.alive; }
static BOOL WINAPI FakeVisible(HWND h) { auto it = popups.find(h); return it != popups.end() ? it->second : FakeIsWindow(h); }
static BOOL WINAPI FakeShow(HWND h, int cmd) {
    auto it = popups.find(h);
    if (it != popups.end()) { it->second = cmd != SW_HIDE; ++popupShows; } else if (Known(h)) ++sourceTouched;
    return TRUE;
}
static DWORD WINAPI FakePid(HWND h, LPDWORD pid) { if (pid) *pid = FakeIsWindow(h) ? sources[h].pid : 0; return 1; }
static BOOL WINAPI FakeIconic(HWND h) { auto it = sources.find(h); return it != sources.end() && it->second.minimized; }
static BOOL WINAPI FakeAffinity(HWND h, DWORD* value) {
    ++affinityAsks;
    auto it = sources.find(h); *value = it != sources.end() && it->second.protectedContent ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE; return TRUE;
}
static HRESULT WINAPI FakeAttribute(HWND, DWORD, PVOID value, DWORD) { *static_cast<DWORD*>(value) = 0; return S_OK; }
static HRESULT WINAPI FakeRegister(HWND destination, HWND source, HTHUMBNAIL* out) {
    if (failRegister || !FakeIsWindow(source)) return E_FAIL;
    ++registrations; *out = (HTHUMBNAIL)(nextThumbnail++); live[*out] = source; liveDest[*out] = destination; return S_OK;
}
static HRESULT WINAPI FakeUnregister(HTHUMBNAIL h) {
    if (!live.erase(h)) { ++strayUnregisters; return E_INVALIDARG; }   // never registered, or released twice
    liveDest.erase(h); lastProps.erase(h); return S_OK;
}
static HRESULT WINAPI FakeSize(HTHUMBNAIL h, SIZE* size) {
    ++sizeAsks;
    auto it = live.find(h);
    if (it == live.end()) return E_INVALIDARG;
    auto s = sources.find(it->second);
    if (s == sources.end() || !s->second.alive || s->second.dwmFails) return E_FAIL;
    *size = s->second.dwm; return S_OK;
}
static HRESULT WINAPI FakeUpdate(HTHUMBNAIL h, const DWM_THUMBNAIL_PROPERTIES* props) {
    ++propUpdates;
    if (!live.count(h) || props->rcDestination.right <= props->rcDestination.left ||
        props->rcDestination.bottom <= props->rcDestination.top) { ++strayUpdates; return E_INVALIDARG; }
    auto s = sources.find(live[h]);
    if (s != sources.end() && s->second.updateFails) return E_FAIL;
    lastProps[h] = *props; return S_OK;
}
static BOOL WINAPI FakeWindowRect(HWND h, LPRECT r) {
    auto it = sources.find(h);
    if (it == sources.end()) return FALSE;
    *r = it->second.minimized ? it->second.minimizedRect : it->second.normalRect; return TRUE;
}
static int WINAPI FakeClassName(HWND, LPWSTR name, int count) { wcscpy_s(name, count, L"FixtureClass"); return 12; }
static BOOL WINAPI FakePos(HWND h, HWND, int x, int y, int cx, int cy, UINT) {
    if (Known(h)) ++sourceTouched;
    if (popups.count(h)) { ++popupMoves; popupAt[h] = {x, y, x + cx, y + cy}; }
    return TRUE;
}
static int WINAPI FakeRegion(HWND, HRGN r, BOOL) { ++regions; DeleteObject(r); return 1; }
static BOOL WINAPI FakeInvalidate(HWND, const RECT*, BOOL) { ++invalidates; return TRUE; }
static int WINAPI FakeText(HWND, LPWSTR text, int count) { wcscpy_s(text, count, L"Fixture window"); return 14; }
static HWND WINAPI FakeForeground() { return front; }
static SHORT WINAPI FakeKey(int) { return 0; }
static BOOL WINAPI FakeDestroy(HWND) { return TRUE; }
static LONG_PTR WINAPI FakeUserData(HWND, int) { return reinterpret_cast<LONG_PTR>(current); }
static HWND WINAPI FakeCapture(HWND h) { HWND old = captured; captured = h; return old; }
static HWND WINAPI FakeCaptured() { return captured; }
static BOOL WINAPI FakeRelease() { captured = nullptr; return TRUE; }
static void FakeLog(const wchar_t*, ...) {}

#define IsWindow FakeIsWindow
#define IsWindowVisible FakeVisible
#define ShowWindow FakeShow
#define GetWindowThreadProcessId FakePid
#define IsIconic FakeIconic
#define GetWindowDisplayAffinity FakeAffinity
#define DwmGetWindowAttribute FakeAttribute
#define DwmRegisterThumbnail FakeRegister
#define DwmUnregisterThumbnail FakeUnregister
#define DwmQueryThumbnailSourceSize FakeSize
#define DwmUpdateThumbnailProperties FakeUpdate
#define GetWindowRect FakeWindowRect
#define GetClassNameW FakeClassName
#define SetWindowPos FakePos
#define SetWindowRgn FakeRegion
#define InvalidateRect FakeInvalidate
#define GetWindowTextW FakeText
#define GetForegroundWindow FakeForeground
#define GetAsyncKeyState FakeKey
#define DestroyWindow FakeDestroy
#define GetWindowLongPtrW FakeUserData
#define SetCapture FakeCapture
#define GetCapture FakeCaptured
#define ReleaseCapture FakeRelease
#define Log FakeLog
#include "../src/window_preview.cpp"
#undef Log

AppModel g_apps;
void AppModel::ActivateWindow(HWND) { ++activations; }
void AppModel::AskToClose(HWND h, const std::wstring&) {
    asked.push_back(h);
    if (dismissWhileAsking && current) current->Dismiss();   // e.g. the helper's question took the front
}

static std::shared_ptr<AppItem> MakeApp(int count, INT_PTR base, bool keep = false) {
    auto app = std::make_shared<AppItem>(); app->name = L"Fixture";
    if (!keep) sources.clear();
    for (int i = 0; i < count; ++i) { HWND h = (HWND)(base + i); sources[h] = {}; app->windows.push_back(h); }
    return app;
}
static HWND Popup(INT_PTR h) { popups[(HWND)h] = false; return (HWND)h; }
static LPARAM Center(RECT r) { return MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2); }
static bool Inside(RECT inner, RECT outer) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
}
// Every relationship the preview holds belongs to a card on its page, and every such card's handle is held.
static bool Balanced(const WindowPreview& p) {
    size_t held = 0;
    for (int i = 0; i < (int)p.entries.size(); ++i) {
        const auto& e = p.entries[i];
        if (!e.thumbnail) continue;
        ++held;
        auto it = live.find(e.thumbnail);
        if (it == live.end() || it->second != e.source || liveDest[e.thumbnail] != p.hwnd) return false;
        if (i < p.first || i >= p.first + p.capacity) return false;
    }
    size_t mine = 0;
    for (auto& [t, d] : liveDest) mine += d == p.hwnd;
    return held == mine;
}

// Every relationship held shows its window's picture: DWM has its properties, the picture inside its card's, as dim as the card.
static bool Drawn(const WindowPreview& p) {
    for (const auto& e : p.entries) {
        if (!e.thumbnail) continue;
        auto it = lastProps.find(e.thumbnail);
        if (it == lastProps.end() || !it->second.fVisible || !Inside(it->second.rcDestination, e.image) ||
            it->second.opacity != (e.closing ? kClosingOpacity : 255)) return false;
    }
    return true;
}

// The dock's own reveal (common.h's Spring, as Dock::Update steps it) and the two thresholds it uses for
// the preview: the icon under the pointer from 90 % of the way up (dock.cpp, hover), the preview allowed
// from 95 % (dock.cpp, WindowPreview::Update's last argument).
constexpr float kDockHoverFrom = 0.9f, kDockPreviewFrom = 0.95f;

int main() {
    const RECT mon{0, 0, 2560, 1440};
    const RECT anchor{1256, 1340, 1304, 1440};
    const POINT onIcon{1280, 1400};

    // ---- opening: the dock slides up under the pointer
    for (double hz : {60.0, 144.0, 240.0}) {
        auto app = MakeApp(2, 0x1000);
        WindowPreview p; p.hwnd = Popup(0x8080);
        Spring reveal;
        double t = 0;
        // the pointer pushed to the bottom edge: the dock is hidden, then comes up; the pointer is on an icon by then
        for (int i = 0; i < 30; ++i) { p.Update(nullptr, nullptr, anchor, mon, Edge::Bottom, 1, onIcon, t, false); t += 1 / hz; }
        reveal.target = 1;
        double allowedAt = -1, openedAt = -1;
        for (int i = 0; i < (int)(1.2 * hz); ++i) {
            reveal.step((float)(1 / hz), 230, 0.72f);
            auto hovered = reveal.x > kDockHoverFrom ? app : nullptr;
            const bool allowed = reveal.x > kDockPreviewFrom;
            if (allowed && allowedAt < 0) allowedAt = t;
            p.Update(nullptr, hovered, anchor, mon, Edge::Bottom, 1, onIcon, t, allowed);
            if (p.Open() && openedAt < 0) openedAt = t;
            t += 1 / hz;
        }
        char what[160];
        std::snprintf(what, sizeof(what), "the preview opens for the icon the pointer rests on as the dock comes up (%.0f Hz)", hz);
        Check(p.Open(), what);
        std::snprintf(what, sizeof(what), "...a hover delay after the dock is up, not sooner (%.0f Hz)", hz);
        Check(allowedAt >= 0 && openedAt >= allowedAt + kHoverDelay - 1 / hz - 1e-6 && openedAt <= allowedAt + kHoverDelay + 1 / hz + 1e-6, what);
        p.Dismiss();
    }
    {   // not allowed for a while before it opened, with no press (a fullscreen app in front, the dock not up yet):
        // once allowed, as any hover. (A menu of the dock is no such case: it opens from a press, which dismisses
        // the preview, and the dock's Update does not run during its modal loop.)
        auto app = MakeApp(2, 0x1300);
        WindowPreview p; p.hwnd = Popup(0x8089);
        double t = 0;
        for (int i = 0; i < 60; ++i, t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, false);
        Check(!p.Open(), "not allowed: no preview, however long the pointer rests on the icon");
        const double from = t;
        while (!p.Open() && t < from + 2) { p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true); t += 0.016; }
        Check(p.Open() && t - from >= kHoverDelay, "...allowed again (the fullscreen app gone): it opens a hover delay later");
        // Esc or a click outside put it away: away until the pointer leaves, a pause of the preview in between or not
        p.Dismiss();
        for (int i = 0; i < 6; ++i, t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, false);
        for (int i = 0; i < 60; ++i, t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        Check(!p.Open(), "put away (Esc): it stays away under the resting pointer, through a pause of the preview too");
        p.Update(nullptr, nullptr, anchor, mon, Edge::Bottom, 1, {1280, 900}, t, true); t += 0.016;
        for (int i = 0; i < 40; ++i, t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        Check(p.Open(), "...and comes again once the pointer left the icon and came back");
        p.Dismiss();
    }
    {   // a press on the icon is not a hover: the preview waits for the pointer to leave it, as before
        auto app = MakeApp(2, 0x1100);
        WindowPreview p; p.hwnd = Popup(0x8081);
        double t = 0;
        for (; t < 0.6; t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        Check(p.Open(), "fixture: the preview open after the hover delay");
        for (int i = 0; i < 6; ++i, t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, false);   // pressed
        for (int i = 0; i < 60; ++i, t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        Check(!p.Open(), "after a press on the icon the preview stays away while the pointer stays on it");
        p.Update(nullptr, nullptr, anchor, mon, Edge::Bottom, 1, {1280, 900}, t, true); t += 0.016;
        for (int i = 0; i < 40; ++i, t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        Check(p.Open(), "...and comes again once the pointer left the icon and came back");
        p.Dismiss();
        Check(live.empty(), "dismissed: no relationship held");
    }

    {   // a press before the preview opened: the dock dismisses the preview on every press (Dock::WndProc)
        auto app = MakeApp(2, 0x1200);
        WindowPreview p; p.hwnd = Popup(0x8088);
        double t = 0;
        for (; t < 0.3; t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        p.Dismiss();   // WM_LBUTTONDOWN on the icon
        for (int i = 0; i < 6; ++i, t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, false);
        for (int i = 0; i < 60; ++i, t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        Check(!p.Open(), "a press on the icon before its preview opened: none while the pointer stays on it");
    }

    // ---- two docks: one preview at a time
    {
        auto a = MakeApp(2, 0x2000), b = MakeApp(3, 0x2100, true);
        WindowPreview p, q; p.hwnd = Popup(0x8082); q.hwnd = Popup(0x8083);
        const RECT side{-1440, -560, 0, 2000}, sideAnchor{-744, 1900, -696, 2000};
        double t = 0;
        auto pass = [&](std::shared_ptr<AppItem> overP, std::shared_ptr<AppItem> overQ, POINT at) {
            p.Update(nullptr, overP, anchor, mon, Edge::Bottom, 1, at, t, true);
            q.Update(nullptr, overQ, sideAnchor, side, Edge::Bottom, 1, at, t, true);
            t += 0.016;
        };
        while (t < 0.6) pass(a, nullptr, onIcon);
        Check(p.Open() && !q.Open() && live.size() == 2, "fixture: the main dock's preview open");
        for (int i = 0; i < 40; ++i) pass(nullptr, b, {-720, 1960});
        Check(q.Open() && !p.Open(), "the other dock's preview opening closes the first one");
        Check(live.size() == 3 && Balanced(p) && Balanced(q), "...which lets go of its windows' relationships");
        for (int i = 0; i < 40; ++i) pass(a, nullptr, onIcon);
        Check(p.Open() && !q.Open() && live.size() == 2 && Balanced(p), "back on the first dock: its preview again, the other one gone");
        p.Dismiss(); q.Dismiss();
        Check(live.empty(), "both dismissed: nothing held");
    }

    // ---- layout next to icons at both ends of the dock, on every edge, at the users' scales
    struct Screen { RECT mon; float scale; const char* name; };
    const Screen screens[] = {
        {{0, 0, 2560, 1440}, 1.0f, "2560x1440 at 1.0"},
        {{-1440, -560, 0, 2000}, 1.0f, "portrait 1440x2560 at negative coordinates"},
        {{0, 0, 2880, 1800}, 1.25f, "2880x1800 at 1.25 (a laptop)"},
        {{2880, -300, 5440, 1140}, 1.0f, "2560x1440 right of a laptop, raised"},
        {{0, 0, 1366, 768}, 0.72f, "1366x768 at 0.72"},
        {{0, 0, 3840, 2160}, 2.5f, "3840x2160 at 2.5"},
    };
    for (const Screen& s : screens) {
        for (Edge edge : {Edge::Bottom, Edge::Top, Edge::Left, Edge::Right}) {
            bool onMonitor = true, clear = true, cardsIn = true, partsIn = true, reach = true, hits = true, balanced = true;
            const RECT& m = s.mon;
            const int icon = (int)std::lround(48 * s.scale), depth = (int)std::lround(100 * s.scale);
            for (int place = 0; place < 3; ++place)
            for (int count : {1, 3, 12, 40}) {
                auto app = MakeApp(count, 0x3000);
                WindowPreview p; p.hwnd = Popup(0x8084); popups[p.hwnd] = true;
                p.app = app; p.monitor = m; p.edge = edge; p.scale = s.scale;
                const bool vertical = IsVertical(edge);
                const int along0 = vertical ? m.top : m.left, along1 = vertical ? m.bottom : m.right;
                const int at = place == 0 ? along0 + 2 : place == 1 ? (along0 + along1) / 2 - icon / 2 : along1 - 2 - icon;
                switch (edge) {
                case Edge::Bottom: p.anchor = {at, m.bottom - depth, at + icon, m.bottom}; break;
                case Edge::Top: p.anchor = {at, m.top, at + icon, m.top + depth}; break;
                case Edge::Left: p.anchor = {m.left, at, m.left + depth, at + icon}; break;
                case Edge::Right: p.anchor = {m.right - depth, at, m.right, at + icon}; break;
                }
                p.Refresh();
                for (int page = 0; page < count; ++page) {
                    const RECT b = p.bounds, a = p.anchor;
                    onMonitor &= Inside(b, m);
                    clear &= edge == Edge::Bottom ? b.bottom <= a.top : edge == Edge::Top ? b.top >= a.bottom :
                             edge == Edge::Left ? b.left >= a.right : b.right <= a.left;
                    const RECT client{0, 0, b.right - b.left, b.bottom - b.top};
                    for (int i = p.first; i < std::min(p.first + p.capacity, count); ++i) {
                        const auto& e = p.entries[i];
                        cardsIn &= Inside(e.card, client) && e.card.right > e.card.left && e.card.bottom > e.card.top;
                        RECT x = p.CloseRect(e), both{};
                        partsIn &= Inside(e.image, e.card) && Inside(x, e.card) && !IntersectRect(&both, &x, &e.image) &&
                                   e.image.right > e.image.left && e.image.bottom > e.image.top;
                        POINT c{(e.card.left + e.card.right) / 2, (e.image.top + e.image.bottom) / 2};
                        POINT xc{(x.left + x.right) / 2, (x.top + x.bottom) / 2};
                        hits &= p.Hit(c) == i && p.HitClose(xc) == i && p.HitClose(c) == -1;
                    }
                    // straight from the middle of the icon to the popup, the pointer never leaves the preview's reach
                    POINT from{(a.left + a.right) / 2, (a.top + a.bottom) / 2};
                    POINT to{std::clamp<LONG>(from.x, b.left, b.right - 1), std::clamp<LONG>(from.y, b.top, b.bottom - 1)};
                    for (int k = 0; k <= 64; ++k) {
                        POINT pt{from.x + (to.x - from.x) * k / 64, from.y + (to.y - from.y) * k / 64};
                        reach &= p.Contains(pt);
                    }
                    balanced &= Balanced(p);
                    int before = p.first; p.ChangePage(1); if (before == p.first) break;
                }
                p.Dismiss(); balanced &= live.empty();
            }
            const char* edgeName = edge == Edge::Bottom ? "bottom" : edge == Edge::Top ? "top" : edge == Edge::Left ? "left" : "right";
            char what[200];
            auto say = [&](bool ok, const char* rule) { std::snprintf(what, sizeof(what), "%s, dock at the %s: %s", s.name, edgeName, rule); Check(ok, what); };
            say(onMonitor, "the popup stays on the dock's monitor next to icons at either end");
            say(clear, "the popup stays clear of the dock's icon");
            say(cardsIn, "every card lies inside the popup");
            say(partsIn, "each card's picture and close button lie inside it, apart");
            say(hits, "each card, and its close button, answer for their own window");
            say(reach, "from the icon straight to the popup the pointer stays in the preview's reach");
            say(balanced, "relationships only for the page shown, all let go on dismiss");
        }
    }

    // ---- a card that is closing: its close button rests (a second press, a double click, asks nothing more)
    {
        auto app = MakeApp(2, 0x4000);
        WindowPreview p; p.hwnd = Popup(0x8085); popups[p.hwnd] = true;
        p.app = app; p.monitor = mon; p.anchor = anchor; p.edge = Edge::Bottom; p.scale = 1; p.foreground = front;
        p.Refresh(); current = &p; asked.clear();
        const LPARAM x0 = Center(p.CloseRect(p.entries[0]));
        WindowPreview::Proc(p.hwnd, WM_LBUTTONDOWN, 0, x0); WindowPreview::Proc(p.hwnd, WM_LBUTTONUP, 0, x0);
        Check(asked.size() == 1 && p.entries[0].closing, "fixture: the first window asked to close");
        const int before = activations;
        // where the close button was: while the window closes no button is drawn there, only the card
        WindowPreview::Proc(p.hwnd, WM_LBUTTONDOWN, 0, x0); WindowPreview::Proc(p.hwnd, WM_LBUTTONUP, 0, x0);
        Check(asked.size() == 1, "a click where the resting close button was asks nothing again");
        Check(activations == before && p.Open(), "...nor brings the window that is closing up: the popup stays");
        const LPARAM card0 = Center(p.entries[0].image);
        WindowPreview::Proc(p.hwnd, WM_LBUTTONDOWN, 0, card0); WindowPreview::Proc(p.hwnd, WM_LBUTTONUP, 0, card0);
        Check(activations == before + 1 && !p.Open(), "a click on the rest of a closing card still brings its window up");
        p.Dismiss();
        // the AppModel's close takes a while (the administrator helper's question) and the popup goes meanwhile
        app = MakeApp(3, 0x4100);
        p.app = app; popups[p.hwnd] = true; p.Refresh(); dismissWhileAsking = true;
        const LPARAM x1 = Center(p.CloseRect(p.entries[1]));
        WindowPreview::Proc(p.hwnd, WM_LBUTTONDOWN, 0, x1); WindowPreview::Proc(p.hwnd, WM_LBUTTONUP, 0, x1);
        dismissWhileAsking = false;
        Check(asked.size() == 2 && !p.Open() && p.entries.empty() && live.empty(), "a popup gone while its close was asked stays gone, holding nothing");
        current = nullptr;
    }

    // ---- the wheel turns a page a notch at a time. A precision touchpad (a laptop's) or a free-spinning wheel
    // sends a notch in smaller steps: WM_MOUSEWHEEL's documented contract is to add them up to WHEEL_DELTA.
    {
        auto app = MakeApp(40, 0x6000);
        WindowPreview p; p.hwnd = Popup(0x8087); popups[p.hwnd] = true;
        p.app = app; p.monitor = mon; p.anchor = anchor; p.edge = Edge::Bottom; p.scale = 1; p.foreground = front;
        p.Refresh(); current = &p;
        auto wheel = [&](int delta) { WindowPreview::Proc(p.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(SHORT)delta), 0); };
        Check(p.capacity == 11 && p.first == 0, "fixture: 40 windows, 11 cards a page, on the first page");
        wheel(-WHEEL_DELTA);
        Check(p.first == 11, "a notch down: the next page");
        wheel(WHEEL_DELTA);
        Check(p.first == 0, "a notch up: back to the first");
        Check(Drawn(p), "...its pictures shown again where their cards are (new relationships, their properties sent anew)");
        wheel(WHEEL_DELTA);
        Check(p.first == 0, "a notch up on the first page: stays");
        wheel(-30);
        Check(p.first == 0, "a touchpad step (a quarter notch) alone turns no page");
        wheel(-30); wheel(-30); wheel(-30);
        Check(p.first == 11, "...four of them, a notch in all, turn one page, not four");
        while (p.first < 22) p.ChangePage(1);
        while (p.first > 22) p.ChangePage(-1);
        Check(p.first == 22, "fixture: the third page");
        for (int i = 0; i < 8; ++i) wheel(15);
        Check(p.first == 11, "a notch up in eight steps: one page back");
        wheel(-90); wheel(WHEEL_DELTA);
        Check(p.first == 0, "most of a notch down, then a notch up: a page up (the part turned the other way is let go)");
        wheel(-WHEEL_DELTA - WHEEL_DELTA / 2);
        Check(p.first == 11, "a notch and a half down in one message: one page");
        wheel(-WHEEL_DELTA / 2);
        Check(p.first == 22, "...and the half left over, with another half: the next");
        wheel(-90); p.Dismiss(); p.app = app; popups[p.hwnd] = true; p.Refresh();
        wheel(-30);
        Check(p.first == 0, "a part of a notch goes with the popup: opened again, a quarter notch turns no page");
        Check(Balanced(p), "...the page shown holds its relationships, no other");
        p.Dismiss(); current = nullptr;
        Check(live.empty(), "dismissed: nothing held");
    }

    // ---- the dock hidden after a preview: it comes by on every pass (allowed false) and there is nothing to put away
    {
        auto app = MakeApp(2, 0x7000);
        WindowPreview p; p.hwnd = Popup(0x808A); current = &p;
        double t = 0;
        for (; t < 0.6; t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        Check(p.Open(), "fixture: a preview opened once");
        const int shows = popupShows;
        // an hour at the main loop's resting pace (10 passes a second), the dock slid away
        for (int i = 0; i < 36000; ++i, t += 0.1) p.Update(nullptr, nullptr, anchor, mon, Edge::Bottom, 1, {1280, 900}, t, false);
        std::printf("note: an hour of passes with the dock hidden after a preview: %d ShowWindow calls on the popup\n", popupShows - shows);
        Check(!p.Open() && popupShows - shows == 1, "an hour with the dock hidden: the popup hidden once, not again on every pass");
        Check(live.empty() && !popups[p.hwnd], "...holding nothing, shown nowhere");
        // a hide that did not take: put away on the next pass, as before
        popups[p.hwnd] = true;
        p.Update(nullptr, nullptr, anchor, mon, Edge::Bottom, 1, {1280, 900}, t, false); t += 0.1;
        Check(!popups[p.hwnd], "a popup still shown with nothing in it: hidden on the next pass");
        // a press queued for the popup reaches it after it went: the capture it took is let go on the next pass
        WindowPreview::Proc(p.hwnd, WM_LBUTTONDOWN, 0, MAKELPARAM(20, 20));
        Check(captured == p.hwnd, "fixture: a late press took the pointer's capture for the hidden popup");
        p.Update(nullptr, nullptr, anchor, mon, Edge::Bottom, 1, {1280, 900}, t, false); t += 0.1;
        Check(captured == nullptr && p.pressed == nullptr && p.pressedNav == 0, "...let go on the next pass, nothing pressed");
        // and it opens as before once the dock is up again under the pointer
        const double from = t;
        while (!p.Open() && t < from + 2) { p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true); t += 0.016; }
        Check(p.Open() && t - from >= kHoverDelay, "...the dock up again: the preview opens a hover delay later");
        p.Dismiss(); current = nullptr;
        Check(live.empty(), "dismissed: nothing held");
    }

    // ---- the icon under the open popup grows and slides with the magnification, every frame while the pointer
    // moves on it: the popup moves along, its cards are not arranged or asked about again until the next Refresh
    for (float scale : {1.0f, 1.5f}) {
        char what[200];
        const RECT big{0, 0, 2880, 1800};
        const int icon = (int)std::lround(48 * scale), depth = (int)std::lround(100 * scale), mid = 1440;
        auto anchorAt = [&](int dx, int grow) { return RECT{mid - icon / 2 + dx - grow, big.bottom - depth, mid + icon / 2 + dx + grow, big.bottom}; };
        auto app = MakeApp(5, 0x7100);
        WindowPreview p; p.hwnd = Popup(0x808B); current = &p;
        const POINT on{mid, big.bottom - depth / 2};
        double t = 0;
        for (; t < 0.6; t += 1.0 / 144) p.Update(nullptr, app, anchorAt(0, 0), big, Edge::Bottom, scale, on, t, true);
        Check(p.Open() && p.entries.size() == 5, "fixture: the popup open with five cards");
        const int shows0 = sizeAsks + affinityAsks, updates0 = propUpdates, regions0 = regions, moves0 = popupMoves, inval0 = invalidates;
        // a second at 144 Hz: the icon's middle swings up to 30 px either way and it grows by up to 12 px a side
        for (int i = 0; i < 144; ++i, t += 1.0 / 144) {
            const int dx = (int)std::lround(30 * std::sin(i * 0.15)), grow = (int)std::lround(6 + 6 * std::cos(i * 0.1));
            p.Update(nullptr, app, anchorAt(dx, grow), big, Edge::Bottom, scale, on, t, true);
        }
        std::snprintf(what, sizeof(what), "note: a second of the icon moving under the popup at %.1f: %d window/DWM questions, %d property updates, "
                      "%d regions, %d moves, %d repaints", scale, sizeAsks + affinityAsks - shows0, propUpdates - updates0, regions - regions0,
                      popupMoves - moves0, invalidates - inval0);
        std::printf("%s\n", what);
        std::snprintf(what, sizeof(what), "a second of the icon moving under the popup (%.1f): the popup moved along on every frame", scale);
        Check(popupMoves - moves0 >= 100, what);
        std::snprintf(what, sizeof(what), "...its region, picture and DWM properties left as they were, the windows asked only by the 0.2 s Refresh (%.1f)", scale);
        Check(regions == regions0 && invalidates == inval0 && propUpdates == updates0 && sizeAsks + affinityAsks - shows0 <= 6 * 2 * 5, what);
        // where it went is where a full arrangement puts it, with the same cards and pictures
        const RECT followed = p.bounds, at = popupAt[p.hwnd], bridged = p.bridge;
        std::vector<RECT> cards, images;
        std::map<HTHUMBNAIL, RECT> drawn;
        for (auto& e : p.entries) { cards.push_back(e.card); images.push_back(e.image); if (e.thumbnail) drawn[e.thumbnail] = lastProps[e.thumbnail].rcDestination; }
        p.Arrange();
        bool same = SameRect(followed, p.bounds) && SameRect(at, p.bounds) && SameRect(bridged, p.bridge);
        for (size_t i = 0; i < p.entries.size(); ++i) {
            const auto& e = p.entries[i];
            same &= SameRect(cards[i], e.card) && SameRect(images[i], e.image) && e.fallback.empty() && e.thumbnail &&
                    drawn.count(e.thumbnail) && SameRect(drawn[e.thumbnail], lastProps[e.thumbnail].rcDestination);
        }
        std::snprintf(what, sizeof(what), "...where it lies, its cards and their pictures are what a full arrangement makes (%.1f)", scale);
        Check(same && Balanced(p), what);
        // a window that changes meanwhile: its card shows it by the next Refresh, as while the icon stays still
        sources[app->windows[2]].protectedContent = true;
        const double changed = t;
        while (t < changed + 0.5 && p.entries[2].thumbnail) {
            const int dx = (int)std::lround(30 * std::sin(t * 20));
            p.Update(nullptr, app, anchorAt(dx, 6), big, Edge::Bottom, scale, on, t, true); t += 1.0 / 144;
        }
        std::snprintf(what, sizeof(what), "...a window protected meanwhile: its card says so within 0.2 s, its picture let go (%.1f)", scale);
        Check(!p.entries[2].thumbnail && p.entries[2].fallback == L"미리보기가 제한된 창" && t - changed <= 0.2 + 2.0 / 144, what);
        sources[app->windows[2]].protectedContent = false;
        p.Dismiss(); current = nullptr;
    }
    {   // a move that changes the popup's size (a small monitor: the icon's height takes from the cards): arranged in full
        const RECT tiny{0, 0, 600, 330};
        auto app = MakeApp(3, 0x7200);
        WindowPreview p; p.hwnd = Popup(0x808C); popups[p.hwnd] = true;
        p.app = app; p.monitor = tiny; p.edge = Edge::Bottom; p.scale = 1; p.anchor = {276, 180, 324, 330}; p.foreground = front;
        p.Refresh();
        const LONG tall = p.bounds.bottom - p.bounds.top;
        const int regions0 = regions;
        p.anchor = {270, 200, 330, 330}; p.Arrange(true);
        bool inside = p.bounds.bottom <= p.anchor.top;
        for (int i = p.first; i < std::min(p.first + p.capacity, (int)p.entries.size()); ++i)
            inside &= Inside(p.entries[i].image, p.entries[i].card) && p.entries[i].card.bottom <= p.bounds.bottom - p.bounds.top;
        Check(p.bounds.bottom - p.bounds.top != tall && regions == regions0 + 1 && inside && popupAt[p.hwnd].bottom == p.bounds.bottom,
              "an icon move that resizes the popup: cards, pictures and region made anew, all inside it");
        p.Dismiss();
        Check(live.empty(), "dismissed: nothing held");
    }

    // ---- an open popup at rest: the 0.2 s Refresh asks about the windows, DWM is sent only what changed
    {
        auto app = MakeApp(5, 0x7300);
        WindowPreview p; p.hwnd = Popup(0x808D); current = &p;
        double t = 0;
        for (; t < 0.6; t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        Check(p.Open(), "fixture: the popup open with five cards");
        const int updates0 = propUpdates, asks0 = sizeAsks;
        for (const double end = t + 10; t < end; t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        std::printf("note: ten seconds of an open popup at rest: %d thumbnail property updates, %d source sizes asked\n",
                    propUpdates - updates0, sizeAsks - asks0);
        Check(propUpdates == updates0 && sizeAsks - asks0 >= 5 * 45, "ten seconds at rest: the windows looked at every 0.2 s, DWM sent nothing new");
        HWND resized = app->windows[3];
        sources[resized].dwm = {1080, 1920};   // turned upright
        const int before = propUpdates;
        for (const double end = t + 0.3; t < end; t += 0.016) p.Update(nullptr, app, anchor, mon, Edge::Bottom, 1, onIcon, t, true);
        const auto& e = p.entries[3];
        const RECT d = lastProps[e.thumbnail].rcDestination;
        Check(propUpdates == before + 1 && d.bottom - d.top > d.right - d.left && Inside(d, e.image),
              "a window resized: its picture's new shape sent by the next Refresh, once, inside its card");
        p.Dismiss(); current = nullptr;
        Check(live.empty(), "dismissed: nothing held");
    }

    // ---- long random use: windows come, go, minimize, get protected, hand their handle on; pages turn
    {
        std::mt19937 rng(20261028);
        auto pick = [&](int n) { return (int)(rng() % (unsigned)n); };
        auto app = MakeApp(30, 0x5000);
        for (int i = 30; i < 36; ++i) sources[(HWND)(INT_PTR)(0x5000 + i)] = {};   // windows the app does not have yet
        WindowPreview p; p.hwnd = Popup(0x8086); popups[p.hwnd] = true;
        p.app = app; p.monitor = {0, 0, 1366, 768}; p.anchor = {659, 668, 707, 768}; p.edge = Edge::Bottom; p.scale = 0.72f;
        p.foreground = front; current = &p;
        p.Refresh();
        int unbalancedAt = -1, strayAt = -1, touchedAt = -1, undrawnAt = -1;
        const int strayBefore = strayUnregisters, updatesBefore = strayUpdates;
        const SIZE sizes[] = {{1920, 1080}, {160, 28}, {0, 0}, {300, 188}, {2896, 1720}, {480, 84}};
        for (int step = 0; step < 6000; ++step) {
            HWND h = (HWND)(INT_PTR)(0x5000 + pick(36));
            Source& s = sources[h];
            switch (pick(16)) {
            case 0: s.minimized = !s.minimized; break;
            case 1: s.dwm = sizes[pick(6)]; break;
            case 2: s.protectedContent = !s.protectedContent; break;
            case 3: s.alive = !s.alive; break;
            case 4: s.pid = 3 + pick(3); break;   // the handle went to another program, or came back
            case 5: s.dwmFails = !s.dwmFails; break;
            case 6: s.updateFails = !s.updateFails; break;
            case 7: failRegister = pick(4) == 0; break;
            case 8: p.ChangePage(pick(2) ? 1 : -1); break;
            case 9: {   // the AppModel's list changes
                auto it = std::find(app->windows.begin(), app->windows.end(), h);
                if (it != app->windows.end()) app->windows.erase(it); else app->windows.push_back(h);
                break;
            }
            case 10: if (!p.entries.empty()) p.AskClose(pick((int)p.entries.size())); break;
            case 11: p.anchor.left = 600 + pick(120); p.anchor.right = p.anchor.left + 48; p.Arrange((step & 1) != 0); break;   // the icon moving: followed, or arranged in full
            case 12:   // dismissed and opened again
                p.Dismiss(); p.app = app; popups[p.hwnd] = true; break;
            case 13: {   // a click on a card's close button or a middle click on a card
                if (p.entries.empty() || !p.app) break;
                int i = p.first + pick(std::max(1, std::min(p.capacity, (int)p.entries.size() - p.first)));
                if (i >= (int)p.entries.size()) break;
                LPARAM at = pick(2) ? Center(p.CloseRect(p.entries[i])) : Center(p.entries[i].image);
                UINT down = pick(2) ? WM_LBUTTONDOWN : WM_MBUTTONDOWN;
                if (down == WM_LBUTTONDOWN && at == Center(p.entries[i].image)) break;   // a pick of a window ends the round
                WindowPreview::Proc(p.hwnd, down, 0, at); WindowPreview::Proc(p.hwnd, down + 1, 0, at);
                break;
            }
            default: break;
            }
            if (p.app) p.Refresh();
            if (p.entries.empty() && p.app) { p.Dismiss(); p.app = app; popups[p.hwnd] = true; p.Refresh(); }
            if (unbalancedAt < 0 && !Balanced(p)) unbalancedAt = step;
            if (undrawnAt < 0 && !Drawn(p)) undrawnAt = step;
            if (strayAt < 0 && (strayUnregisters != strayBefore || strayUpdates != updatesBefore)) strayAt = step;
            if (touchedAt < 0 && sourceTouched) touchedAt = step;
        }
        failRegister = false;
        p.Dismiss();
        if (unbalancedAt >= 0) std::printf("note: first unbalanced step %d\n", unbalancedAt);
        if (strayAt >= 0) std::printf("note: first stray DWM call at step %d\n", strayAt);
        Check(unbalancedAt < 0, "6000 random steps: every relationship held belongs to a card on the page shown, and each such card has one");
        Check(strayAt < 0, "...no relationship is released twice or updated after its release");
        if (undrawnAt >= 0) std::printf("note: first relationship without its picture at step %d\n", undrawnAt);
        Check(undrawnAt < 0, "...each relationship held shows its window's picture inside its card, as dim as the card");
        Check(live.empty(), "...and after the last dismiss nothing is held");
        Check(touchedAt < 0, "...no window is shown, moved or restored for its preview");
        Check(registrations > 1000, "fixture: the random steps registered relationships often");
        current = nullptr;
    }
    return Summary("preview_rules_test");
}
