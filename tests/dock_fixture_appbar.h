#pragma once
// The shell's app bar list, as Explorer keeps it (lost when Explorer restarts): the dock tests' SHAppBarMessage
// (dock_layout_test checks it, the other dock tests name FakeAppBar in their #define list so no real app bar is ever
// registered - a real one would shrink the work area of the user's screen). Include it after the test's Windows
// headers (<shellapi.h>), before the Dock's parts.
// The work area the dock checks its strip against (Dock::CheckAppBar) is worked out from the strips this shell took:
// LD_APPBAR_WORK_AREA below, so no test reads the real screen's work area for it.
#include <algorithm>
#include <map>
struct FakeShell {
    bool running = true;           // Explorer is there; without it SHAppBarMessage finds no taskbar and fails
    HWND taskbar = (HWND)(uintptr_t)0x7A5C0001;   // Explorer's taskbar window (a new one for every Explorer)
    std::vector<HWND> bars;        // registered app bars
    struct Strip { RECT rc; UINT edge; };
    std::map<HWND, Strip> strips;  // the strip each registered bar took (ABM_SETPOS); left out of the work area
    // The fault of mod.36 and before: the dock's own ABM_QUERYPOS/ABM_SETPOS went through its tray host, and Explorer,
    // handed a block it could not read, took no strip while saying TRUE (tray_host.cpp, ForwardAppBar).
    bool takesNoStrip = false;
    int news = 0, removes = 0, setPos = 0, unregisteredCalls = 0;   // news: every ABM_NEW call, failed ones too
    RECT lastPos{};
    void Reset() { *this = FakeShell{}; }
    bool Has(HWND h) const { return std::find(bars.begin(), bars.end(), h) != bars.end(); }
    // the monitor's work area: what the strips on it leave (each cuts its edge back to its inner side)
    RECT Work(const RECT& mon) const {
        RECT w = mon;
        for (const auto& [h, s] : strips) {
            RECT on{};
            if (!IntersectRect(&on, &s.rc, &mon)) continue;
            switch (s.edge) {
            case ABE_BOTTOM: w.bottom = std::min(w.bottom, s.rc.top); break;
            case ABE_TOP:    w.top = std::max(w.top, s.rc.bottom); break;
            case ABE_LEFT:   w.left = std::max(w.left, s.rc.right); break;
            case ABE_RIGHT:  w.right = std::min(w.right, s.rc.left); break;
            }
        }
        return w;
    }
} shell;
static UINT_PTR WINAPI FakeAppBar(DWORD msg, PAPPBARDATA abd) {
    if (msg == ABM_NEW) ++shell.news;
    if (!shell.running) return FALSE;
    switch (msg) {
    case ABM_NEW:
        if (shell.Has(abd->hWnd)) return FALSE;   // already registered
        shell.bars.push_back(abd->hWnd); return TRUE;
    case ABM_REMOVE:
        ++shell.removes;
        shell.strips.erase(abd->hWnd);
        shell.bars.erase(std::remove(shell.bars.begin(), shell.bars.end(), abd->hWnd), shell.bars.end()); return TRUE;
    case ABM_QUERYPOS:
        if (!shell.Has(abd->hWnd)) { ++shell.unregisteredCalls; return FALSE; }
        return TRUE;
    case ABM_SETPOS:
        if (!shell.Has(abd->hWnd)) { ++shell.unregisteredCalls; return FALSE; }
        ++shell.setPos; shell.lastPos = abd->rc;
        if (!shell.takesNoStrip) shell.strips[abd->hWnd] = { abd->rc, abd->uEdge };
        return TRUE;
    }
    return FALSE;
}
static bool FakeAppBarWorkArea(HMONITOR, const RECT& mon, RECT& work) { work = shell.Work(mon); return true; }
#define LD_APPBAR_WORK_AREA FakeAppBarWorkArea
