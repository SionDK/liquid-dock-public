// The opacity guard on Explorer's taskbars (src/taskbar_visibility.h) on its own, with the window calls
// faked: what it refuses to touch (another program's property, a window drawn with UpdateLayeredWindow),
// Explorer putting its opacity back while the dock runs, a failed attempt rolled back, and a recovery that
// only has the window properties (the dock crashed). tests/taskbar_recovery_test.cpp drives it through
// NativeTaskbar; this test reaches the paths that one does not. No real window is touched.
//   cl /std:c++20 /EHsc /utf-8 tests\tray_visibility_test.cpp
#include "../src/common.h"
#include "portable_check.h"
#include <map>

namespace fake {
struct Wnd {
    LONG_PTR ex = WS_EX_TOOLWINDOW;
    bool attrs = false;            // SetLayeredWindowAttributes was used (else: none, or UpdateLayeredWindow)
    COLORREF color = 0; BYTE alpha = 255; DWORD flags = 0;
    std::map<std::wstring, HANDLE> props;
};
std::map<HWND, Wnd> w;
bool failSetLayered = false, failSetProp = false;
}

static BOOL WINAPI FakeIsWindow(HWND h) { return fake::w.count(h) ? TRUE : FALSE; }
static LONG_PTR WINAPI FakeGetWindowLongPtrW(HWND h, int) { return fake::w.count(h) ? fake::w[h].ex : 0; }
static LONG_PTR WINAPI FakeSetWindowLongPtrW(HWND h, int, LONG_PTR v) {
    if (!fake::w.count(h)) return 0;
    auto& s = fake::w[h];
    LONG_PTR old = s.ex;
    if ((old & WS_EX_LAYERED) && !(v & WS_EX_LAYERED)) s.attrs = false;   // Windows forgets the attributes
    s.ex = v;
    return old;
}
static HANDLE WINAPI FakeGetPropW(HWND h, LPCWSTR n) {
    if (!fake::w.count(h)) return nullptr;
    auto& p = fake::w[h].props;
    auto it = p.find(n);
    return it == p.end() ? nullptr : it->second;
}
static BOOL WINAPI FakeSetPropW(HWND h, LPCWSTR n, HANDLE v) {
    if (fake::failSetProp || !fake::w.count(h)) return FALSE;
    fake::w[h].props[n] = v;
    return TRUE;
}
static HANDLE WINAPI FakeRemovePropW(HWND h, LPCWSTR n) {
    HANDLE v = FakeGetPropW(h, n);
    if (fake::w.count(h)) fake::w[h].props.erase(n);
    return v;
}
static BOOL WINAPI FakeGetLayeredWindowAttributes(HWND h, COLORREF* c, BYTE* a, DWORD* f) {
    if (!fake::w.count(h)) return FALSE;
    auto& s = fake::w[h];
    if (!(s.ex & WS_EX_LAYERED) || !s.attrs) return FALSE;
    if (c) *c = s.color;
    if (a) *a = s.alpha;
    if (f) *f = s.flags;
    return TRUE;
}
static BOOL WINAPI FakeSetLayeredWindowAttributes(HWND h, COLORREF c, BYTE a, DWORD f) {
    if (fake::failSetLayered || !fake::w.count(h)) return FALSE;
    auto& s = fake::w[h];
    if (!(s.ex & WS_EX_LAYERED)) return FALSE;
    s.attrs = true; s.color = c; s.alpha = a; s.flags = f;
    return TRUE;
}
#define IsWindow FakeIsWindow
#define GetWindowLongPtrW FakeGetWindowLongPtrW
#define SetWindowLongPtrW FakeSetWindowLongPtrW
#define GetPropW FakeGetPropW
#define SetPropW FakeSetPropW
#define RemovePropW FakeRemovePropW
#define GetLayeredWindowAttributes FakeGetLayeredWindowAttributes
#define SetLayeredWindowAttributes FakeSetLayeredWindowAttributes
#include "../src/taskbar_visibility.h"

using namespace taskbarvisibility;

int main() {
    const HWND bar = (HWND)0x100;
    auto reset = [&] { fake::w.clear(); fake::w[bar] = {}; fake::failSetLayered = fake::failSetProp = false; };
    auto plain = [&] { const auto& s = fake::w[bar]; return !(s.ex & WS_EX_LAYERED) && s.props.empty(); };

    reset();
    Check(Protect(bar) && IsProtected(bar) && fake::w[bar].alpha == 0 && (fake::w[bar].flags & LWA_ALPHA), "a plain taskbar: made transparent");
    Check(Restore(bar) && plain(), "...and back: not layered, no properties left");

    reset();
    fake::w[bar].props[kState] = (HANDLE)(ULONG_PTR)0x7;   // another program's value under the name
    Check(!Protect(bar) && !(fake::w[bar].ex & WS_EX_LAYERED), "a property the dock did not write: not touched");
    Check(Restore(bar) && fake::w[bar].props.count(kState), "...and not removed by a restore");

    reset();
    fake::w[bar].ex |= WS_EX_LAYERED;   // drawn with UpdateLayeredWindow: no attributes to put back
    Check(!Protect(bar) && fake::w[bar].props.empty() && !fake::w[bar].attrs, "a window drawn with UpdateLayeredWindow: refused, unchanged");

    reset();
    Check(Protect(bar), "protected");
    fake::w[bar].alpha = 255;   // Explorer puts its opacity back while the dock runs
    Check(!IsProtected(bar) && Protect(bar) && IsProtected(bar), "Explorer made it opaque again: transparent again");
    Check(Restore(bar) && plain(), "...and the restore goes back to the state before the dock, not to Explorer's 255");

    reset();
    fake::w[bar].ex |= WS_EX_LAYERED;
    fake::w[bar].attrs = true; fake::w[bar].color = 0x00ff00; fake::w[bar].alpha = 0; fake::w[bar].flags = LWA_COLORKEY;
    Check(Protect(bar) && fake::w[bar].flags == LWA_ALPHA, "a colour-keyed taskbar: made transparent");
    Check(Restore(bar) && fake::w[bar].color == 0x00ff00 && fake::w[bar].flags == LWA_COLORKEY && (fake::w[bar].ex & WS_EX_LAYERED) &&
          fake::w[bar].props.empty(), "...and its colour key comes back exactly");

    reset();
    fake::failSetLayered = true;
    Check(!Protect(bar) && plain(), "the opacity cannot be set: rolled back, nothing left behind");

    reset();
    fake::failSetProp = true;
    Check(!Protect(bar) && plain(), "no room for the backup: nothing changed");

    reset();
    Check(Protect(bar), "protected (the dock then crashes)");
    fake::failSetLayered = true;   // not needed for a window that was not layered
    Check(Restore(bar) && plain(), "a recovery that has only the window's properties puts it back");

    reset();
    fake::w[bar].ex |= WS_EX_LAYERED;
    fake::w[bar].attrs = true; fake::w[bar].alpha = 200; fake::w[bar].flags = LWA_ALPHA;
    Check(Protect(bar), "a half-transparent taskbar: protected");
    fake::failSetLayered = true;
    Check(!Restore(bar) && fake::w[bar].props.count(kState), "its opacity cannot be put back yet: the backup stays for a retry");
    fake::failSetLayered = false;
    Check(Restore(bar) && fake::w[bar].alpha == 200 && fake::w[bar].props.empty(), "the retry puts back 200");

    reset();
    Check(Protect(bar), "protected (then Explorer restarts)");
    fake::w.erase(bar);
    Check(Restore(bar), "a window that is gone: nothing to restore");

    // NativeTaskbar::StartEnforcer's passes (EnforcerPlan): its wake-ups as they are - every 250 ms in the first
    // minute, auto-hide asked on every pass then; later every second, asked on every fifth pass (and on a recheck,
    // which the enforcer takes itself after the sleep).
    {
        const EnforcerPass first = EnforcerPlan(0, 0), lastEarly = EnforcerPlan(59.99, 239);
        Check(first.sleepMs == 250 && first.alwaysAsk && lastEarly.sleepMs == 250 && lastEarly.alwaysAsk,
              "enforcer: the first minute, 250 ms and auto-hide asked on every pass");
        const EnforcerPass at60 = EnforcerPlan(60, 241), fifth = EnforcerPlan(60.25, 245), later = EnforcerPlan(3600, 14401);
        Check(at60.sleepMs == 1000 && !at60.alwaysAsk && later.sleepMs == 1000 && !later.alwaysAsk,
              "enforcer: from 60 s, a second, and auto-hide not asked as such...");
        Check(fifth.sleepMs == 1000 && fifth.alwaysAsk && EnforcerPlan(7200, 0).alwaysAsk, "...except on every fifth pass");
        int asked = 0;
        for (int pass = 1000; pass < 1100; ++pass) asked += EnforcerPlan(600, pass).alwaysAsk ? 1 : 0;
        Check(asked == 20, "enforcer: later on, one pass in five asks");
        double t = 0;
        int passes = 0, asks = 0;
        for (int pass = 0; t < 120; ++pass) {
            const EnforcerPass p = EnforcerPlan(t, pass);
            t += p.sleepMs / 1000.0;
            ++passes;
            if (p.alwaysAsk) ++asks;
        }
        Check(passes == 300 && asks == 252, "enforcer: its first two minutes, 300 wake-ups (240 + 60) and 252 auto-hide questions");
    }
    return Summary("tray_visibility_test");
}
