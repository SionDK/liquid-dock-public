#pragma once
// What a click on an app's dock icon does (AppModel::Click in apps_click.cpp), kept free of Windows headers
// so it can be tested with any C++20 compiler (tests/app_click_test.cpp, run by tests/run_portable.sh).
//
// Explorer's taskbar minimizes a window on a click only when that window is the active one and is not
// minimized (ReactOS CTaskSwitchWnd::HandleTaskItemClick, modelled on Windows); every other click
// brings the window up. A minimized window can stay the active one - nothing else took the activation
// when it went down - and the dock used to "minimize" it again: the first click did nothing, the next
// one (once something else was active) brought it up.
//
// A program slow to answer (seen on a test PC, 2026-10-26: still minimized 0.45 s after the
// click) gets clicked again before its window comes. Those clicks bring the same window up again: they
// never minimize it - the one right after it came would - and never start over the dock's wait for it
// (each click used to restart the 0.45 s before the second try, so it never came while the user kept
// clicking). What the dock does while it waits: Next() below.
#include <cstdint>
#include <vector>

namespace appclick {

// One of the app's windows as the dock sees it at the click. The app's list is in z-order, topmost
// first, and was made up to a second earlier.
struct Win {
    uint64_t id = 0;
    bool shown = true;          // visible and not cloaked now (it may have been hidden since the list was made)
    bool minimized = false;
    bool onScreen = true;       // not minimized, larger than a pixel and on a monitor
    bool clickThrough = false;  // layered and transparent to the mouse: an overlay drawn over other windows
                                // (a game overlay, say), never the window to bring up
};

enum class Action { Launch, Minimize, Activate };

enum class Why {
    NoWindow,          // no window left that can be shown: start it (an app that runs once shows its window)
    Active,            // its only window is the active one: minimize it
    Cycle,             // one of its windows is active: bring up the bottom-most other one that shows (on a screen, or
                       // minimized: restoring shows it); one off the screens only when no other shows
    ActiveMinimized,   // the active window is its own but minimized: bring it back
    Front,             // bring up its topmost window on a screen
    Minimized,         // all its windows are minimized: restore the topmost
    OffScreen,         // its windows are all off the screens: bring up the topmost anyway
    Again,             // the dock is still bringing one of its windows up (or just did): that one again
};

struct Decision {
    Action action = Action::Launch;
    uint64_t window = 0;
    Why why = Why::NoWindow;
    int skipped = 0;    // windows passed over because they are no longer shown
    int overlays = 0;   // click-through overlays passed over
    int offScreen = 0;  // windows off the screens a cycle passed over (bringing one up would show nothing)
};

// pending: the window of this app the dock is still bringing up (a click came before it did);
// justUp: the one it brought up kJustUp seconds ago or less. 0: none.
inline Decision Decide(const std::vector<Win>& wins, uint64_t foreground, uint64_t pending = 0, uint64_t justUp = 0) {
    Decision d;
    std::vector<const Win*> usable;
    for (const auto& w : wins) {
        if (!w.shown) ++d.skipped;
        else if (w.clickThrough) ++d.overlays;
        else usable.push_back(&w);
    }
    if (usable.empty()) return d;
    if (pending)   // a click while it is coming: the same window, whatever its state now
        for (const Win* w : usable)
            if (w->id == pending) { d.action = Action::Activate; d.window = pending; d.why = Why::Again; return d; }
    const Win* fg = nullptr;
    if (foreground)
        for (const Win* w : usable)
            if (w->id == foreground) { fg = w; break; }
    if (fg && !fg->minimized) {
        if (usable.size() == 1) {
            if (fg->id == justUp) {   // the click right after it came: not a wish to minimize it
                d.action = Action::Activate; d.window = fg->id; d.why = Why::Again;
                return d;
            }
            d.action = Action::Minimize; d.window = fg->id; d.why = Why::Active;
            return d;
        }
        // the bottom-most other one that shows when brought up, as Front prefers one; else the bottom-most other
        // (one of several is never minimized)
        const Win* next = nullptr;
        const Win* away = nullptr;   // the bottom-most other one off the screens
        int passed = 0;
        for (auto it = usable.rbegin(); it != usable.rend() && !next; ++it) {
            if (*it == fg) continue;
            if ((*it)->onScreen || (*it)->minimized) next = *it;
            else { if (!away) away = *it; ++passed; }
        }
        if (next || away) {
            d.action = Action::Activate; d.window = (next ? next : away)->id; d.why = Why::Cycle;
            d.offScreen = next ? passed : 0;
            return d;
        }
    }
    d.action = Action::Activate;
    if (fg) { d.window = fg->id; d.why = Why::ActiveMinimized; return d; }
    for (const Win* w : usable)
        if (w->onScreen && !w->minimized) { d.window = w->id; d.why = Why::Front; return d; }
    for (const Win* w : usable)
        if (w->minimized) { d.window = w->id; d.why = Why::Minimized; return d; }
    d.window = usable.front()->id; d.why = Why::OffScreen;
    return d;
}

// A window brought up has come: shown, not minimized, and the active window is that window or another
// one of the same program (an app may pass the activation on to a window of its own) - but not the one
// that was in front at the click already (foregroundWasThere): one program may hold several of the dock's
// icons (a browser's profiles, the frame host of the store apps), and its other window staying in front
// is no sign that this one came.
inline bool CameUp(bool shown, bool minimized, bool foregroundIsIt, bool foregroundSameProgram, bool foregroundWasThere = false) {
    return shown && !minimized && (foregroundIsIt || (foregroundSameProgram && !foregroundWasThere));
}

// ---- after the click: does the window come? (AppModel::CheckWatch)
//
// A minimized window's restore command (WM_SYSCOMMAND SC_RESTORE, as from Explorer's taskbar) is sent
// from a worker thread, so the dock knows when the program has handled it and never waits on it
// itself. While the program has not answered it is busy: the dock sends nothing more - a second
// restore after the first would take a window that comes back maximized out of maximized - and waits
// up to kBusyWait. A program that handled the command and left the window down gets the system's own
// restore (ShowWindowAsync) at once, on that worker.
constexpr double kLook = 0.25;          // between looks (s)
constexpr double kQuietFlash = 3.0;     // a window the dock tried to put in front: its flashes (Windows refusing
                                        // the dock) are no attention request for this long after each try
constexpr double kBusyNote = 0.5;       // no answer this long: the log says the program is busy
constexpr double kBusyWait = 8.0;       // how long a busy program may take before the dock gives up
constexpr double kAfterRestore = 0.5;   // after the program handled the restore: time for the window to show
constexpr double kJustUp = 0.6;         // a click this soon after the window came up never minimizes it
constexpr int kRefocusTries = 2;        // up but behind another window: put in front this many times

struct Look {
    double since = 0;           // since the first click of this bring-up
    bool gone = false;          // the window no longer exists
    bool cameUp = false;        // CameUp(...)
    bool elsewhere = false;     // another program came in front since the click: the user went on
    bool restoring = false;     // its restore command is not handled yet: the program is busy
    bool failed = false;        // the command could not be delivered (refused, or no answer in kBusyWait)
    double sinceHandled = -1;   // since the program handled the restore command (-1: none sent)
    bool minimized = false;
    bool shown = true;
    int refocused = 0;          // times the dock put it in front again
};

enum class Step {
    Done,      // it came
    Stop,      // gone, or the user went on to another program: nothing more to do
    Wait,      // look again in kLook
    Restore,   // minimized and no restore command sent (it went down after the click): send one
    Refocus,   // up but not in front: put it in front again
    GiveUp,    // it did not come: a line in the log with what the dock saw
};

inline Step Next(const Look& l) {
    if (l.gone) return Step::Stop;
    if (l.cameUp) return Step::Done;
    if (l.elsewhere) return Step::Stop;
    if (l.restoring) return l.since < kBusyWait ? Step::Wait : Step::GiveUp;
    if (l.failed) return Step::GiveUp;
    if (l.minimized || !l.shown) {
        if (l.sinceHandled < 0) return l.minimized ? Step::Restore : Step::GiveUp;   // hidden by its program: nothing to restore
        return l.sinceHandled < kAfterRestore ? Step::Wait : Step::GiveUp;
    }
    return l.refocused < kRefocusTries ? Step::Refocus : Step::GiveUp;
}

// ---- a shell pop-up in front at the click (AppModel::BringUp, CheckWatch)
//
// Start, Search, quick settings: Windows keeps one of these in front of any window the dock asks for (it
// only flashes the window - 2026-10-29: seven clicks with SearchHost in front, the dock
// holding the right to change the front window), and a click on the dock, which never takes activation,
// leaves it open where a click on the taskbar closes it. So the dock closes it first, with Esc (the key
// that closes each of them), only while it is still in front - a key for the window behind it would be
// a key in that program - and brings the window up once it has gone, as at a click.
constexpr double kPopupLook = 0.1;      // between looks while it closes
constexpr double kPopupGone = 0.5;      // Esc this long ago and still in front: Esc again
constexpr int kPopupTries = 2;          // Esc this many times, then give up

enum class PopupStep {
    Gone,      // not in front any more: bring the window up now
    Wait,      // closing: look again in kPopupLook
    Close,     // still in front: Esc
    GiveUp,    // it stayed: a line in the log
};

// stillInFront: in front and shown - one in front but cloaked (closed, or on its way out) has nothing to close.
inline PopupStep NextForPopup(bool stillInFront, int closed, double sinceClosed) {
    if (!stillInFront) return PopupStep::Gone;
    if (closed > 0 && sinceClosed < kPopupGone) return PopupStep::Wait;
    return closed < kPopupTries ? PopupStep::Close : PopupStep::GiveUp;
}

inline const wchar_t* WhyText(Why w) {
    switch (w) {
    case Why::NoWindow: return L"no window to show: starting it";
    case Why::Active: return L"its window is the active one: minimizing it";
    case Why::Cycle: return L"one of its windows is active: bringing up the next";
    case Why::ActiveMinimized: return L"its minimized window is still the active one: bringing it back";
    case Why::Front: return L"bringing up its window";
    case Why::Minimized: return L"restoring its minimized window";
    case Why::OffScreen: return L"its windows are off the screens: bringing up the first";
    case Why::Again: return L"clicked again while its window is coming: bringing the same one up (not minimizing it)";
    }
    return L"";
}

}   // namespace appclick
