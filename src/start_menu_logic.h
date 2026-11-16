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
// A click presses the Start button of Explorer's own taskbar - hidden while the dock runs, there all the
// same (taskbar_start.h) - so the dock's button does what the taskbar's does: Explorer handles the press.
// Until that has brought Start once on this Windows build, the dock checks it: when Start is not up
// kTaskbarProof after the press, the click goes on as without the taskbar button; kTaskbarMissesOff presses in
// a row that clearly did not bring Start, and the taskbar button is left alone (for kVerdictDays). Once it
// has brought Start, a press is trusted like a request the shell took (below) - that verdict is kept across
// runs, so right after a restart, when Start can take seconds, no check runs out and stops it - and a click
// with Start open presses it too. A press is trusted for the click too when Explorer is evidently busy (a single
// call into it took kBusyCall) or the press may still be on its way (a timeout): another request could stop it.
//
// Without the taskbar's button - none to be had, or it did not work here - a click asks the shell for Start
// (IImmersiveLauncher::ShowStartView, the request Explorer's own Start button makes; RetroBar and
// ExplorerPatcher use it the same way) on the dock's monitor and watches whether Start comes. One request
// per click: after the shell took it, nothing more is sent - neither the Windows key (it toggles, and would
// close a Start still on its way) nor another request (2026-10-27 11:43: three requests
// within 0.2 s, all taken, and Start never came; ExplorerPatcher asks only once too, and only while Start
// is not up). More clicks while waiting are counted, nothing else. The key is pressed only when the shell
// cannot be asked or refuses, at most once per click; a refused request may be repeated, since nothing is
// on its way then. What worked is remembered for the next click (Preference).
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

// Times in seconds. A click's timeline starts once the dock is ready to send (the taskbar's button found,
// or the shell reached), so a slow Explorer at logon does not use up the wait before anything was sent.
constexpr double kLook = 0.03;            // between looks while waiting
constexpr double kTaskbarProof = 2.0;     // a press on the taskbar's button that has not brought Start yet: the
                                          // other ways go out when Start is not up this much later
constexpr double kBusyCall = 0.25;        // a single call into Explorer this slow (pressing the taskbar's button,
                                          // asking for Start's visibility): it is busy, and Start will be slow too
constexpr double kShellAgain = 1.0;       // the shell refused: ask again this much later (nothing is on its way then)
constexpr double kShellAfterKey = 0.6;    // the key, when it reaches Start, brings it within a few frames
constexpr double kGiveUp = 4.0;           // after the first request, when no request was taken: then a line in
                                          // the log says what the dock saw
constexpr double kWaitAccepted = 8.0;     // after the shell took a request (or the trusted taskbar button was
                                          // pressed): it brings Start, however slowly
constexpr int kMaxShell = 3;              // requests to the shell per click (only refused ones are repeated)
constexpr double kStuck = 15.0;           // a click's worker still running after this long (from the click) waits
                                          // on a hung Explorer: the next click starts over instead of joining it
                                          // (the one left behind sends nothing more once Explorer answers it,
                                          // nor asks it anything)

// Is an earlier click still bringing Start up? (Its worker runs, and has not been stuck for kStuck.)
inline bool StillOpening(bool workerRunning, double workerAge) { return workerRunning && workerAge < kStuck; }

// How a click uses the taskbar's own Start button.
enum class TaskbarUse : uint8_t {
    None,       // not: no button to be had for the dock's monitor, or it did not work here
    Unproven,   // pressed first; it has not brought Start yet: kTaskbarProof later the other ways go out
    Proven,     // pressed first; it has brought Start before: trusted like a request the shell took
};

// One click that should bring Start up, from the moment the dock is ready to send (t = 0) until Start is
// there or the dock gives up.
class Opening {
public:
    // shellFirst: ask the shell before pressing the Windows key (Preference). shellReady: the shell can be
    // asked for this click. keyBlocked: the key cannot reach Start now - a program that runs as administrator
    // is in front, and Windows drops injected input that would go to it. taskbar: the taskbar's button first.
    Opening(bool shellFirst, bool shellReady, bool keyBlocked, TaskbarUse taskbar = TaskbarUse::None)
        : shellReady_(shellReady), keyBlocked_(keyBlocked), taskbar_(taskbar),
          other_(shellReady && (shellFirst || keyBlocked) ? Way::Shell : Way::Key) {}

    // What to do at t (seconds on the click's timeline), Start being visible or not. Way::None while
    // !Done(): look again after kLook. The first request always goes out, however late the first look.
    Way Next(double t, bool visible) {
        if (done_) return Way::None;
        if (visible) return Finish(t, true);
        if (taskbar_ != TaskbarUse::None) {
            if (!taskbarSent_) return Way::Taskbar;
            if (taskbarOk_) {
                if (TaskbarTrusted())   // it brings Start, however slowly: nothing more is sent
                    return t >= taskbarAt_ + kWaitAccepted ? Finish(t, false) : Way::None;
                if (t < taskbarAt_ + kTaskbarProof) return Way::None;   // still proving itself
            }
            // the press did not go through, or has not brought Start in time: the other ways, as without it
        }
        if (!shellSent_ && !keySent_) return other_;
        if (t >= GiveUpAt()) return Finish(t, false);
        return other_ == Way::Shell ? AfterShell(t) : AfterKey(t);
    }

    // The request went out: t = when the call returned (asking the shell can block), ok = the shell
    // answered S_OK / every key event was inserted / the taskbar's button took the press.
    void Sent(Way w, double t, bool ok) {
        if (w == Way::None) return;
        if (w == Way::Taskbar) {
            taskbarSent_ = true; taskbarOk_ = ok; taskbarAt_ = t;
            if (ok) last_ = w;   // a press that did not go through brings nothing
            return;
        }
        if (firstAt_ < 0) firstAt_ = t;
        if (w == Way::Shell) {
            ++shellSent_; shellOk_ = ok; shellAt_ = t;
            if (ok && acceptedAt_ < 0) acceptedAt_ = t;
        } else { ++keySent_; keyOk_ = ok; keyAt_ = t; }
        last_ = w;
    }

    // Explorer is evidently busy on this click (a single call into it took kBusyCall or more): a press that
    // went through is waited on like a trusted one - Start will be slow as well, and the shell's request or
    // the key could stop it on its way.
    void Busy() { busy_ = true; }

    // The press failed in a way that may still reach Explorer (a timeout): waited on like a trusted one, as a
    // late press plus another request would toggle Start twice; nothing is learned from it. Sent(Taskbar, t,
    // true) goes with it.
    void Uncertain() { uncertain_ = true; }

    // The shell could not be reached after all (made only when needed, behind the taskbar's button): as if it
    // had not been ready - the key instead, and nothing to learn about the shell.
    void ShellLost() {
        shellReady_ = false;
        if (!shellSent_ && !keySent_) other_ = Way::Key;
    }

    // Another click while Start is on its way: counted for the log; nothing is sent for it (a second
    // request, like the key, can stop the Start on its way).
    void Again(double) { ++clicks_; }

    // The button closed Start (it was in front at the press): the wait is over, nothing more is sent.
    void Closed(double t) { if (!done_) Finish(t, true); }

    // When the dock stops waiting once the shell or the key went out: kGiveUp after the first of them,
    // kWaitAccepted after the shell took one.
    double GiveUpAt() const {
        double at = (firstAt_ < 0 ? 0 : firstAt_) + kGiveUp;
        if (acceptedAt_ >= 0) at = (std::max)(at, acceptedAt_ + kWaitAccepted);   // (std::max): safe from windows.h's macro
        return at;
    }

    bool Done() const { return done_; }
    bool Opened() const { return opened_; }
    double At() const { return at_; }              // when Start came or the dock gave up
    Way First() const { return taskbar_ != TaskbarUse::None ? Way::Taskbar : other_; }
    Way CameAfter() const { return came_; }        // the last way tried before Start came (None: it was there)
    bool ShellReady() const { return shellReady_; }
    bool KeyBlocked() const { return keyBlocked_; }
    int ShellSent() const { return shellSent_; }
    bool ShellAccepted() const { return acceptedAt_ >= 0; }   // some request to the shell returned S_OK
    bool ShellOk() const { return shellOk_; }      // the last one did
    int KeySent() const { return keySent_; }
    bool KeyOk() const { return keyOk_; }
    int Clicks() const { return clicks_; }         // more clicks while waiting
    TaskbarUse Taskbar() const { return taskbar_; }
    bool TaskbarSent() const { return taskbarSent_; }
    bool TaskbarOk() const { return taskbarOk_; }  // the press went through (or may have: TaskbarUncertain)
    bool TaskbarUncertain() const { return uncertain_; }
    bool ExplorerBusy() const { return busy_; }
    // a press that went through is waited on alone: proven before, Explorer busy, or the press maybe on its way
    bool TaskbarTrusted() const { return taskbar_ == TaskbarUse::Proven || busy_ || uncertain_; }
    double TaskbarAt() const { return taskbarAt_; }
    double OtherAt() const { return firstAt_; }    // when the first of the shell and the key went out (-1: neither)

private:
    Way Finish(double t, bool opened) {
        done_ = true; opened_ = opened; at_ = t;
        if (opened) came_ = last_;
        return Way::None;
    }
    bool CanAskShell() const { return shellReady_ && shellSent_ < kMaxShell; }
    bool CanPressKey() const { return !keyBlocked_ && keySent_ == 0; }

    // The shell was asked first.
    Way AfterShell(double t) {
        if (acceptedAt_ >= 0) return Way::None;   // taken: the shell brings Start, however slowly; nothing more is sent
        // refused every time so far: the key at once; while no key went out, the shell again a little later
        if (CanPressKey()) return Way::Key;
        if (keySent_) return Way::None;           // the key may be bringing Start: nothing more
        if (CanAskShell()) return t - shellAt_ >= kShellAgain ? Way::Shell : Way::None;
        return Finish(t, false);                  // nothing else can bring Start
    }

    // The Windows key was pressed first.
    Way AfterKey(double t) {
        if (keySent_ && !keyOk_) {   // the key events were not even inserted (input blocked by another thread)
            if (CanAskShell() && !shellSent_) return Way::Shell;
            if (!shellReady_) return Finish(t, false);
        }
        if (acceptedAt_ >= 0) return Way::None;   // the shell took a request: it brings Start; nothing more
        // the key did not bring Start in time (dropped on the way, most likely): the shell, once
        if (CanAskShell() && !shellSent_ && t - keyAt_ >= kShellAfterKey) return Way::Shell;
        // the shell refused: again a little later (nothing of its is on the way)
        if (CanAskShell() && shellSent_ && t - shellAt_ >= kShellAgain) return Way::Shell;
        return Way::None;
    }

    bool shellReady_, keyBlocked_;
    TaskbarUse taskbar_;
    Way other_;                               // the first of the shell and the key
    Way last_ = Way::None, came_ = Way::None;
    bool done_ = false, opened_ = false;
    double at_ = 0;
    int shellSent_ = 0, keySent_ = 0, clicks_ = 0;
    bool shellOk_ = false, keyOk_ = false;
    bool taskbarSent_ = false, taskbarOk_ = false, busy_ = false, uncertain_ = false;
    double firstAt_ = -1, shellAt_ = -1, keyAt_ = -1, acceptedAt_ = -1, taskbarAt_ = -1;
};

// The taskbar's own Start button.
enum class TaskbarState : uint8_t {
    Unknown,   // not proven yet, or it missed once since: pressed first, checked (TaskbarUse::Unproven)
    Works,     // it brought Start: trusted (TaskbarUse::Proven), and closes Start too
    Failed,    // kTaskbarMissesOff presses in a row clearly did not bring Start: left alone
    Off,       // never pressed (the worker test's safe mode)
};
constexpr int kTaskbarMissesOff = 2;

// What the dock learned from earlier clicks (the taskbar button's verdict also from earlier runs).
struct Preference {
    bool shellFirst = true;   // the shell is asked first
    int misses = 0;           // clicks in a row on which the shell was asked and did not bring Start
    bool askShell = true;     // false: the shell is never asked (the worker test's safe mode)
    TaskbarState taskbar = TaskbarState::Unknown;
    int taskbarMisses = 0;    // presses in a row that went through and clearly did not bring Start
    int unclear = 0;          // clicks in a row where the press and the other ways went out and nothing came
    long taskbarFailedDay = -1;   // Failed: the day it was kept (-1: for this run only - GaveUp, Windows' or Explorer's first minutes)
};
constexpr int kMissesKeyFirst = 2;

// The taskbar's button for a click: buttonReady = one is to be had for the dock's monitor.
inline TaskbarUse TaskbarUseFor(const Preference& p, bool buttonReady) {
    if (!buttonReady) return TaskbarUse::None;
    if (p.taskbar == TaskbarState::Works) return TaskbarUse::Proven;
    return p.taskbar == TaskbarState::Unknown ? TaskbarUse::Unproven : TaskbarUse::None;
}

// A click with Start open: the taskbar's button closes it once it has proved itself (as the taskbar's own
// button would) and one is kept for the dock's monitor; else the Windows key, as before.
inline bool ClosesWithTaskbar(const Preference& p) { return p.taskbar == TaskbarState::Works; }

enum class Lesson : uint8_t { None, KeyFirst, ShellFirst };

// After an opening. The shell asked and Start did not come by it (refused; or took the request and
// Start did not come within kWaitAccepted), kMissesKeyFirst clicks in a row: the key goes first from
// now on - the shell is still asked when the key does not bring Start, and first whenever the key
// cannot reach Start. Start brought by the shell: the shell goes first again. A click on which the
// shell could not be asked at all (Explorer not up yet after logon, restarting) teaches nothing: that
// passes; nor does one on which the shell went out behind a press on the taskbar's button that went
// through and did not bring Start: no telling which stopped what (the request can stop a Start the press
// was bringing). Returns what changed, for the log.
inline Lesson Learn(Preference& p, const Opening& o) {
    if (!o.ShellSent()) return Lesson::None;
    if (o.Opened() && o.CameAfter() == Way::Shell) {
        p.misses = 0;
        if (!p.shellFirst) { p.shellFirst = true; return Lesson::ShellFirst; }
        return Lesson::None;
    }
    if (o.TaskbarSent() && o.TaskbarOk()) return Lesson::None;
    if (++p.misses >= kMissesKeyFirst && p.shellFirst) { p.shellFirst = false; return Lesson::KeyFirst; }
    return Lesson::None;
}

enum class TaskbarLesson : uint8_t {
    None,
    Works,      // it brought Start: trusted from now on (kept)
    Unproven,   // a trusted button missed: checked again (the kept verdict stays: one miss is no proof)
    Failed,     // kTaskbarMissesOff clear misses in a row: left alone (kept for kVerdictDays)
    GaveUp,     // kTaskbarMissesOff clicks in a row brought nothing, whatever went out: left alone for this
                // run only (nothing clear enough to keep), so the shell and the key learn as without it
};

// After an opening that pressed the taskbar's button. Start came by it: it works. A miss only on evidence:
// Start came by another way soon after that went out (within kShellAfterKey: Start was warm, the press had
// done nothing - a cold Start that merely came after the fallback proves nothing), or the press was waited
// on alone, Explorer not busy, and Start never came. One miss demotes a trusted button to a checked one;
// kTaskbarMissesOff in a row leave it alone. Nothing is learned from a press that did not go through (no
// button, gone stale) or one that may or may not have arrived (a timeout). When the other ways went out
// too and nothing came there is no telling which stopped what - but kTaskbarMissesOff such clicks in a row
// and the button is left alone for the run, so that a broken shell is still found out (Learn) and the key
// tried, as without the button.
inline TaskbarLesson LearnTaskbar(Preference& p, const Opening& o) {
    if (!o.TaskbarSent() || !o.TaskbarOk() || o.TaskbarUncertain()) return TaskbarLesson::None;
    if (o.Opened()) p.unclear = 0;
    if (o.Opened() && o.CameAfter() == Way::Taskbar) {
        p.taskbarMisses = 0;
        if (p.taskbar == TaskbarState::Works) return TaskbarLesson::None;
        p.taskbar = TaskbarState::Works;
        return TaskbarLesson::Works;
    }
    if (p.taskbar == TaskbarState::Failed || p.taskbar == TaskbarState::Off) return TaskbarLesson::None;
    const bool others = o.ShellSent() || o.KeySent();
    if (!o.Opened() && others) {
        if (++p.unclear < kTaskbarMissesOff) return TaskbarLesson::None;
        p.unclear = 0;
        p.taskbar = TaskbarState::Failed;
        return TaskbarLesson::GaveUp;
    }
    const bool otherBroughtIt = o.Opened() && (o.CameAfter() == Way::Shell || o.CameAfter() == Way::Key) &&
                                o.OtherAt() >= 0 && o.At() - o.OtherAt() <= kShellAfterKey;
    const bool waitedAlone = !o.Opened() && !others && !o.ExplorerBusy();
    if (!otherBroughtIt && !waitedAlone) return TaskbarLesson::None;
    if (++p.taskbarMisses >= kTaskbarMissesOff) { p.taskbar = TaskbarState::Failed; return TaskbarLesson::Failed; }
    if (p.taskbar == TaskbarState::Works) { p.taskbar = TaskbarState::Unknown; return TaskbarLesson::Unproven; }
    return TaskbarLesson::None;
}

// The taskbar button's verdict, kept across runs as "<works|failed> <Windows build> <day>" (start_menu.cpp:
// data\taskbar-start.txt). Another Windows build is checked afresh; a failure is tried again after kVerdictDays.
constexpr long kVerdictDays = 3;
constexpr double kVerdictUptime = 300;   // a failure learned in Windows' or Explorer's first minutes (everything slow) is not kept

// A failure kept on day `when` is over on day `day`: kVerdictDays later, or at once should the clock have gone back.
// The same rule for one read back at a run's first click (ParseVerdict) and for one learned or read earlier in a run
// that lasts days (RetryTaskbar: a laptop sleeps, it does not restart).
inline bool VerdictExpired(long when, long day) { return day < when || day - when >= kVerdictDays; }

// At a click: a kept failure that is over is tried again within the run too, as the next run would - checked
// afresh, from no misses (with the two that left it alone, the first new one would leave it alone again). A
// failure for this run only (taskbarFailedDay -1) stays. Returns whether the button is tried again, for the log.
inline bool RetryTaskbar(Preference& p, long day) {
    if (p.taskbar != TaskbarState::Failed || p.taskbarFailedDay < 0 || !VerdictExpired(p.taskbarFailedDay, day)) return false;
    p.taskbar = TaskbarState::Unknown;
    p.taskbarMisses = 0;
    p.unclear = 0;
    p.taskbarFailedDay = -1;
    return true;
}

// A failure learned right after Windows started is not kept: everything is slow then. Nor right after Explorer
// started - at each logon, also when Windows only woke up: fast startup (Windows' default) turns a shutdown into
// hibernation, and Windows' uptime (GetTickCount64) runs on across it. Seconds; explorerAge < 0: no telling
// (Windows' uptime alone, as before).
inline bool EarlyForVerdict(double uptime, double explorerAge) {
    return uptime < kVerdictUptime || (explorerAge >= 0 && explorerAge < kVerdictUptime);
}

inline std::wstring FormatVerdict(TaskbarState s, const std::wstring& build, long day) {
    if (s != TaskbarState::Works && s != TaskbarState::Failed) return L"";   // nothing kept: checked afresh
    return std::wstring(s == TaskbarState::Works ? L"works " : L"failed ") + build + L" " + std::to_wstring(day);
}

// failedDay (when given): the day a failure read back was kept, for RetryTaskbar.
inline TaskbarState ParseVerdict(const std::wstring& text, const std::wstring& build, long day, long* failedDay = nullptr) {
    const size_t a = text.find(L' ');
    const size_t b = a == std::wstring::npos ? a : text.find(L' ', a + 1);
    if (b == std::wstring::npos || b + 1 >= text.size()) return TaskbarState::Unknown;
    if (text.compare(a + 1, b - a - 1, build) != 0 || build.empty()) return TaskbarState::Unknown;   // another build
    long when = 0;
    for (size_t i = b + 1; i < text.size(); ++i) {
        if (text[i] < L'0' || text[i] > L'9' || when > 100000000) return TaskbarState::Unknown;
        when = when * 10 + (text[i] - L'0');
    }
    if (!text.compare(0, a, L"works")) return TaskbarState::Works;
    if (!text.compare(0, a, L"failed") && !VerdictExpired(when, day)) {
        if (failedDay) *failedDay = when;
        return TaskbarState::Failed;
    }
    return TaskbarState::Unknown;   // an old failure (or the clock went back): checked again
}

inline const wchar_t* WayText(Way w) {
    switch (w) {
    case Way::Taskbar: return L"the taskbar's Start button";
    case Way::Shell: return L"the shell's request";
    case Way::Key: return L"the Windows key";
    case Way::None: break;
    }
    return L"nothing (it was already there)";
}

}   // namespace startmenu
