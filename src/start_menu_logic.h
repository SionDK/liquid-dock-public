#pragma once
// What the dock's start button does (start_menu.cpp), kept free of Windows headers so it can be tested
// with any C++20 compiler (tests/start_menu_test.cpp, run by tests/run_portable.sh).
//
// The button used to press the Windows key for the user (SendInput) and nothing more. Windows drops an
// injected key without a word while a program that runs as administrator is in front (UIPI: "neither
// GetLastError nor the return value will indicate the failure was caused by UIPI blocking"), a keyboard
// hook may swallow injected keys, and right after logon Start can take seconds to come - while the
// Windows key toggles: pressed again while Start is on its way, it closes it. Seen on a test PC on
// 2026-10-27 right after a restart: several clicks on the start button, no Start, the dock working.
//
// A click goes down a ladder of three ways, each at most once (mod.36; a design choice of 2026-11-20):
//   1. it asks the shell for Start (IImmersiveLauncher::ShowStartView, the request Explorer's own Start button
//      makes; RetroBar and ExplorerPatcher use it the same way) on the dock's monitor, and watches whether Start
//      comes;
//   2. kConfirm after the shell took the request and Start is not up, it presses the Start button of Explorer's
//      own taskbar - hidden while the dock runs, there all the same (taskbar_start.h) - and only when that button's
//      toggle state reads off: on, Explorer holds Start open, and a press would close it;
//   3. kGiveUp after the shell took the request, the Windows key - not into an administrator's window.
// A way that was surely not taken (the shell not to be reached, or refusing; no button to press; a press that
// did not go through) has nothing on its way: the next way goes out at once. After a request the shell took,
// nothing is brought forward: with no button to press the key still waits for kGiveUp (it toggles, and would
// close a Start that is merely slow).
//
// From mod.18 to mod.35 the taskbar's button went first, trusted from the first click once it had brought Start
// on a Windows build (a verdict kept in data\taskbar-start.txt) and then waited on alone for kWaitAccepted.
// Twice (2026-11-17, 2026-11-20) such a press went through, the button off -> on, and Start did not come: 8 s
// of nothing while more clicks were only counted. Nothing is remembered between clicks now - no verdict, no
// misses, no order learned: each click's dock.log lines tell all of it.
//
// "Slow" keeps the old protection for a Start that takes seconds: within kEarlyUptime of Windows' or Explorer's
// start, or when a single call into Explorer took kBusyCall on this click, whatever was taken (the shell's
// request, a press) is waited on alone for kWaitAccepted - nothing else is sent, as another request, press or
// key could stop that Start on its way. A press that timed out may still arrive: nothing follows it either.
//
// One request of a kind per click (2026-10-27 11:43: three requests within 0.2 s, all taken,
// and Start never came; ExplorerPatcher asks only once too). More clicks while waiting are counted, nothing else.
#include <algorithm>
#include <cstdint>
#include <string>

namespace startmenu {

enum class Way : uint8_t { None, Shell, Key, Taskbar };

// What a press on the start button means, from what was in front when the button went down.
enum class Press : uint8_t {
    Open,    // bring Start up
    Close,   // Start is in front: the button closes it, as the taskbar's does
    Again,   // Start is still on its way from an earlier click: that click's wait takes this one in (counted, nothing sent)
};

// Start in front closes it, whatever else is going on (a wait still running is stopped: it must not
// bring Start back); otherwise a click joins a wait that is still running.
inline Press OnPress(bool startInFront, bool opening) {
    if (startInFront) return Press::Close;
    return opening ? Press::Again : Press::Open;
}

// Times in seconds. A click's timeline starts once the dock is ready to send (the shell reached, or known
// not to be), so a slow Explorer at logon does not use up the wait before anything was sent.
constexpr double kLook = 0.03;            // between looks while waiting
constexpr double kConfirm = 2.0;          // after something was taken (the shell's request, a press) and Start is
                                          // not up: the next way goes out this much later
constexpr double kBusyCall = 0.25;        // a single call into Explorer this slow (reaching the shell, making
                                          // IAppVisibility, the request, finding or pressing the taskbar's button):
                                          // it is busy, and Start will be slow too
constexpr double kUsual = 0.6;            // Start by the shell's request sooner than this, nothing else going on:
                                          // the usual line in dock.log; otherwise the long one
constexpr double kGiveUp = 4.0;           // the dock gives up on what it sent: this long after the shell took the
                                          // request the Windows key goes out, the last way; and with nothing left
                                          // to send, this long after the last thing taken the wait ends
constexpr double kWaitAccepted = 8.0;     // after the shell took a request the wait ends, whatever else went out;
                                          // a press that went first and is waited on alone: as long
constexpr double kStuck = 15.0;           // a click's worker still running after this long (from the click) waits
                                          // on a hung Explorer: the next click starts over instead of joining it
                                          // (the one left behind sends nothing more once Explorer answers it,
                                          // nor asks it anything)
constexpr double kEarlyUptime = 300;      // Windows or Explorer started less than this ago: everything is slow

// Is an earlier click still bringing Start up? (Its worker runs, and has not been stuck for kStuck.)
inline bool StillOpening(bool workerRunning, double workerAge) { return workerRunning && workerAge < kStuck; }

// Right after Windows started everything is slow, Start too. And right after Explorer started - at each logon,
// also when Windows only woke up: fast startup (Windows' default) turns a shutdown into hibernation, and Windows'
// uptime (GetTickCount64) runs on across it. Seconds; explorerAge < 0: no telling (Windows' uptime alone).
inline bool EarlyAfterStart(double uptime, double explorerAge) {
    return uptime < kEarlyUptime || (explorerAge >= 0 && explorerAge < kEarlyUptime);
}

// One click that should bring Start up, from the moment the dock is ready to send (t = 0) until Start is
// there or the dock gives up.
class Opening {
public:
    // shellReady: the shell can be asked for this click. keyBlocked: the key cannot reach Start now - a
    // program that runs as administrator is in front, and Windows drops injected input that would go to it.
    Opening(bool shellReady, bool keyBlocked) : shellReady_(shellReady), keyBlocked_(keyBlocked) {}

    // What to do at t (seconds on the click's timeline), Start being visible or not. Way::None while
    // !Done(): look again after kLook. The first request always goes out, however late the first look.
    // Way::Taskbar: the button's turn - the worker looks for it then and presses it if its state reads off
    // (Sent), or tells that nothing was pressed (NoPress).
    Way Next(double t, bool visible) {
        if (done_) return Way::None;
        if (visible) return Finish(t, true);
        if (shellReady_ && !shellSent_) return Way::Shell;
        // waited on alone: a press that may still arrive; or, slow, whatever was taken
        if (WaitsAlone()) return t >= AloneUntil() ? Finish(t, false) : Way::None;
        if (!taskbarTurn_) {
            if (ShellAccepted() && t < shellAt_ + kConfirm) return Way::None;   // the shell brings Start: not yet
            return Way::Taskbar;
        }
        if (!keySent_ && !keyBlocked_) return t >= KeyDue() ? Way::Key : Way::None;
        return t >= EndAt() ? Finish(t, false) : Way::None;
    }

    // The request went out: t = when the call returned (asking the shell can block), ok = the shell
    // answered S_OK / every key event was inserted / the taskbar's button took the press.
    void Sent(Way w, double t, bool ok) {
        if (w == Way::None) return;
        if (first_ == Way::None) first_ = w;
        if (ok) { takenAt_ = t; last_ = w; }   // what was not taken (refused, not through, not inserted) brings nothing
        if (w == Way::Taskbar) { taskbarTurn_ = true; taskbarSent_ = true; taskbarOk_ = ok; taskbarAt_ = t; }
        else if (w == Way::Shell) { shellSent_ = true; shellOk_ = ok; shellAt_ = t; }
        else { keySent_ = true; keyOk_ = ok; keyAt_ = t; }
    }

    // The button's turn came and nothing was pressed: none to be had for the dock's monitor, or its toggle
    // state did not read off (on: Explorer holds Start open, and a press would close it).
    void NoPress(double) { taskbarTurn_ = true; }

    // Slow on this click: Windows or Explorer started minutes ago (EarlyAfterStart), or a single call into
    // Explorer took kBusyCall or more. Whatever was taken, or is taken from now on, is waited on alone.
    void Slow() { slow_ = true; }

    // The press failed in a way that may still reach Explorer (a timeout): waited on alone, as a late press
    // plus another request would toggle Start twice. Sent(Taskbar, t, true) goes with it.
    void Uncertain() { uncertain_ = true; }

    // Another click while Start is on its way: counted for the log; nothing is sent for it (a second
    // request, like the key, can stop the Start on its way).
    void Again(double) { ++clicks_; }

    // The button closed Start (it was in front at the press): the wait is over, nothing more is sent.
    void Closed(double t) { if (!done_) Finish(t, true); }

    bool Done() const { return done_; }
    bool Opened() const { return opened_; }
    double At() const { return at_; }              // when Start came or the dock gave up
    Way First() const { return first_; }           // the first way that went out (None: nothing did)
    Way CameAfter() const { return came_; }        // the last way taken before Start came (None: it came by itself)
    bool ShellReady() const { return shellReady_; }
    bool KeyBlocked() const { return keyBlocked_; }
    bool ShellSent() const { return shellSent_; }
    bool ShellAccepted() const { return shellSent_ && shellOk_; }   // the request returned S_OK
    double ShellAt() const { return shellAt_; }
    bool KeySent() const { return keySent_; }
    bool KeyOk() const { return keyOk_; }
    double KeyDue() const {  // when the key may go out: never brought forward behind something that was taken
        double at = 0;
        if (ShellAccepted()) at = shellAt_ + kGiveUp;
        if (taskbarSent_ && taskbarOk_) at = (std::max)(at, taskbarAt_ + kConfirm);   // (std::max): safe from windows.h's macro
        return at;
    }
    int Clicks() const { return clicks_; }         // more clicks while waiting
    bool TaskbarTurn() const { return taskbarTurn_; }   // the button's turn came (pressed or not)
    bool TaskbarSent() const { return taskbarSent_; }
    bool TaskbarOk() const { return taskbarOk_; }  // the press went through (or may have: TaskbarUncertain)
    bool TaskbarUncertain() const { return uncertain_; }
    double TaskbarAt() const { return taskbarAt_; }
    bool IsSlow() const { return slow_; }
    // what was taken is waited on alone, nothing more sent: slow, or the press maybe on its way
    bool WaitsAlone() const { return uncertain_ || (slow_ && (ShellAccepted() || (taskbarSent_ && taskbarOk_))); }

private:
    Way Finish(double t, bool opened) {
        done_ = true; opened_ = opened; at_ = t;
        if (opened) came_ = last_;
        return Way::None;
    }
    // kWaitAccepted from what was taken first: the shell's request, else the press
    double AloneUntil() const { return (ShellAccepted() ? shellAt_ : taskbarAt_) + kWaitAccepted; }
    // nothing left to send: kWaitAccepted after the shell took the request; else kGiveUp after the last thing
    // taken; with nothing taken at all (refused, not through, not inserted) nothing is on its way - at once
    double EndAt() const {
        if (ShellAccepted()) return shellAt_ + kWaitAccepted;
        return takenAt_ < 0 ? 0 : takenAt_ + kGiveUp;
    }

    bool shellReady_, keyBlocked_;
    Way first_ = Way::None, last_ = Way::None, came_ = Way::None;
    bool done_ = false, opened_ = false;
    double at_ = 0;
    int clicks_ = 0;
    bool shellSent_ = false, shellOk_ = false, keySent_ = false, keyOk_ = false;
    bool taskbarTurn_ = false, taskbarSent_ = false, taskbarOk_ = false, slow_ = false, uncertain_ = false;
    double shellAt_ = -1, keyAt_ = -1, taskbarAt_ = -1, takenAt_ = -1;
};

// The taskbar's Start button's toggle state (UI Automation's ToggleState; -1: not read), for the log. A press that
// brought no Start says whether the button was on before it (out of step with Start, which was closed) and whether
// the press flipped it (2026-11-17, a test PC: a trusted press went through
// and Start did not come; the next press brought it in 271 ms). Start's button has two states only: "indeterminate" is
// what UI Automation answers when the button could not tell (its default for a failed read, seen in
// tests\start_menu_taskbar_test.cpp).
inline const wchar_t* ToggleText(int state) {
    switch (state) {
    case 0: return L"off";
    case 1: return L"on";
    case 2: return L"indeterminate";
    default: return L"?";
    }
}

inline const wchar_t* WayText(Way w) {
    switch (w) {
    case Way::Taskbar: return L"the taskbar's Start button";
    case Way::Shell: return L"the shell's request";
    case Way::Key: return L"the Windows key";
    case Way::None: break;
    }
    return L"nothing the dock sent (Start was there by itself)";
}

}   // namespace startmenu
