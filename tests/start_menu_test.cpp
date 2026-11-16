// The dock's start button (src/start_menu_logic.h): press the taskbar's own Start button first - checked
// until it has brought Start, trusted after - else ask the shell for Start once, watch whether it comes,
// press the Windows key only when the shell cannot be asked or refuses, send nothing more after the shell
// took a request (the key toggles, and a second request stopped a Start on its way on a test PC),
// take clicks made while waiting without sending anything, give up with a reason, remember what worked.
// A small simulated Windows drives every case; no Windows headers: builds with any C++20 compiler.
//   cl /std:c++20 /EHsc /utf-8 tests\start_menu_test.cpp
//   g++ -std=c++20 tests/start_menu_test.cpp -o start_menu_test
#include "../src/start_menu_logic.h"
#include "portable_check.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace startmenu;

namespace {

// How the simulated Windows answers.
struct World {
    bool shellReady = true;     // the launcher could be had
    bool shellAccepts = true;   // ShowStartView returns S_OK
    bool shellShows = true;     // ... and Start then comes
    double shellDelay = 0.12;   // after the first accepted request
    bool keyInserted = true;    // SendInput inserted both events
    bool keyReaches = true;     // not dropped on the way (UIPI, a keyboard hook)
    bool keyBlocked = false;    // the dock sees a program that runs as administrator in front
    double keyDelay = 0.08;
    TaskbarUse taskbar = TaskbarUse::None;   // the taskbar's own Start button: found, and how far it is trusted
    bool taskbarTakes = true;   // the press goes through (its pattern answers S_OK)
    bool taskbarUnsure = false; // ...or it times out - and reaches Explorer all the same, late (taskbarTakes ignored)
    bool taskbarShows = true;   // Start then comes
    double taskbarDelay = 0.1;
    bool explorerBusy = false;  // a single call into Explorer took kBusyCall: the worker calls Busy()
    bool launcherFails = false; // the shell, reached only when needed behind the button, cannot be had (ShellLost)
    bool shellStopsPending = true;   // a shell request while a Start is on its way stops it (seen once); false: it does no harm
    bool visibleAtClick = false;
    std::vector<double> clicks; // more clicks while waiting (seconds after the first)
};

struct Run {
    Opening o;
    int shellAsks = 0, keyPresses = 0, taskbarPresses = 0, cancelled = 0;
    double keyAt = -1, firstAcceptAt = -1, lastShellAt = -1, taskbarAt = -1, otherAt = -1;
    Way firstWay = Way::None;          // what went out first
    bool taskbarTook = false;          // a press went through (or may have)
    double startAt = -1;        // when Start shows (-1: not coming)
    bool keyAfterAccept = false;       // a key after the shell took a request (it could close a slow Start)
    bool sentAfterAccept = false;      // anything after the shell took a request (it can stop Start on its way)
    bool sentAfterTrusted = false;     // anything after a press that is waited on alone went through
    bool otherTooSoon = false;         // the shell or the key within kTaskbarProof of a press that went through
    bool shellTooSoon = false;         // a refused request repeated sooner than kShellAgain
    bool actedAfterVisible = false;
    bool shellLost = false;
    double end = 0;
    Run(bool shellFirst, const World& w) : o(shellFirst, w.shellReady, w.keyBlocked, w.taskbar) {}
};

// a press on the taskbar's button is waited on alone: proven, Explorer busy, or the press maybe on its way
bool WaitsAlone(const World& w) { return w.taskbar == TaskbarUse::Proven || w.explorerBusy || w.taskbarUnsure; }

Run Simulate(const World& w, bool shellFirst = true) {
    Run r(shellFirst, w);
    if (w.visibleAtClick) r.startAt = 0;
    if (w.explorerBusy && w.taskbar != TaskbarUse::None) r.o.Busy();   // the worker: a call into Explorer took kBusyCall
    size_t nextClick = 0;
    for (int i = 0; i < 1000 && !r.o.Done(); ++i) {
        double t = i * kLook;
        bool visible = r.startAt >= 0 && t >= r.startAt;
        while (nextClick < w.clicks.size() && w.clicks[nextClick] <= t) { r.o.Again(t); ++nextClick; }
        Way way = r.o.Next(t, visible);
        // the shell, reached only now behind the taskbar's button, cannot be had: the worker tells the opening
        // and asks it again at once
        if (way == Way::Shell && w.launcherFails && w.taskbar != TaskbarUse::None && !r.shellLost) {
            r.shellLost = true;
            r.o.ShellLost();
            way = r.o.Next(t, visible);
        }
        if (way != Way::None && r.firstWay == Way::None) r.firstWay = way;
        if (way != Way::None && visible) r.actedAfterVisible = true;
        if (way != Way::None && r.firstAcceptAt >= 0) r.sentAfterAccept = true;
        if (way != Way::None && r.taskbarTook && WaitsAlone(w)) r.sentAfterTrusted = true;
        if ((way == Way::Shell || way == Way::Key) && r.taskbarTook && t - r.taskbarAt < kTaskbarProof - 1e-9) r.otherTooSoon = true;
        if ((way == Way::Shell || way == Way::Key) && r.otherAt < 0) r.otherAt = t;
        if (way == Way::Taskbar) {
            ++r.taskbarPresses; r.taskbarAt = t;
            const bool arrives = w.taskbarTakes || w.taskbarUnsure;
            if (arrives) {   // like a click on the taskbar's button: a second press while Start comes stops it
                r.taskbarTook = true;
                if (r.startAt >= 0 && t < r.startAt) { r.startAt = -1; ++r.cancelled; }
                else if (w.taskbarShows && r.startAt < 0 && r.cancelled == 0) r.startAt = t + w.taskbarDelay;
            }
            r.o.Sent(Way::Taskbar, t, arrives);
            if (w.taskbarUnsure) r.o.Uncertain();
        } else if (way == Way::Shell) {
            if (r.lastShellAt >= 0 && t - r.lastShellAt < kShellAgain - 1e-9) r.shellTooSoon = true;
            ++r.shellAsks; r.lastShellAt = t;
            bool ok = w.shellAccepts;
            // as seen on a test PC: a request while a Start is on its way stops it, and brings none (or, in the
            // other world, leaves it be)
            if (ok && r.startAt >= 0 && t < r.startAt) { if (w.shellStopsPending) { r.startAt = -1; ++r.cancelled; } }
            else if (ok && w.shellShows && r.startAt < 0 && r.cancelled == 0) r.startAt = t + w.shellDelay;
            if (ok && r.firstAcceptAt < 0) r.firstAcceptAt = t;
            r.o.Sent(Way::Shell, t, ok);
        } else if (way == Way::Key) {
            ++r.keyPresses; r.keyAt = t;
            if (r.firstAcceptAt >= 0) r.keyAfterAccept = true;
            if (w.keyInserted && w.keyReaches && !w.keyBlocked) {
                if (r.startAt >= 0 && t < r.startAt) { r.startAt = -1; ++r.cancelled; }   // toggled shut on its way
                else if (r.startAt < 0) r.startAt = t + w.keyDelay;
            }
            r.o.Sent(Way::Key, t, w.keyInserted);
        }
        r.end = t;
    }
    return r;
}

std::string Describe(const World& w) {
    char b[400];
    std::snprintf(b, sizeof b, "ready=%d accepts=%d shows=%d delay=%.2f inserted=%d reaches=%d blocked=%d clicks=%zu taskbar=%s takes=%d unsure=%d shows=%d delay=%.2f busy=%d lost=%d stops=%d",
                  w.shellReady, w.shellAccepts, w.shellShows, w.shellDelay, w.keyInserted, w.keyReaches, w.keyBlocked, w.clicks.size(),
                  w.taskbar == TaskbarUse::None ? "none" : w.taskbar == TaskbarUse::Unproven ? "unproven" : "proven",
                  w.taskbarTakes, w.taskbarUnsure, w.taskbarShows, w.taskbarDelay, w.explorerBusy, w.launcherFails, w.shellStopsPending);
    return b;
}

}   // namespace

int main() {
    // ---- what a press means
    Check(OnPress(false, false) == Press::Open, "Start not in front: the button opens it");
    Check(OnPress(true, false) == Press::Close, "Start in front: the button closes it, as the taskbar's does");
    Check(OnPress(false, true) == Press::Again, "a click while Start is still coming joins that wait (no second toggle)");
    Check(OnPress(true, true) == Press::Close, "Start in front closes it even while a wait still runs (the wait is stopped)");
    Check(StillOpening(true, 0.5) && StillOpening(true, kWaitAccepted + 1), "a running worker is waited on (it gives up by itself)");
    Check(!StillOpening(true, kStuck + 0.1), "a worker stuck on a hung Explorer is left behind: the next click starts over");
    Check(!StillOpening(false, 0.1), "no worker: a click opens");
    Check(kStuck > kWaitAccepted + 3, "kStuck leaves a slow but working wait alone (and a few seconds to reach the shell)");

    // ---- the timeline: the first request always goes out; waits run from when a call returned
    {
        Opening o(true, true, false);
        Check(o.Next(5.0, false) == Way::Shell, "a first look after kGiveUp (reaching a busy Explorer took long) still asks the shell");
        o.Sent(Way::Shell, 5.2, true);
        Check(o.Next(5.25, false) == Way::None && !o.Done(), "...and then waits for Start instead of giving up at once");
        Check(o.GiveUpAt() >= 5.2 + kWaitAccepted - 1e-9, "...for kWaitAccepted after the shell took the request");
    }
    {
        Opening o(false, true, false);   // key first
        Check(o.Next(6.0, false) == Way::Key, "key first, late first look: the key still goes out");
        o.Sent(Way::Key, 6.0, true);
        Check(o.Next(6.03, false) == Way::None && o.GiveUpAt() >= 6.0 + kGiveUp - 1e-9, "...and the wait runs kGiveUp from it");
    }
    {   // ShowStartView itself took 2.6 s (a busy Explorer) and returned S_OK: no key at any time after it
        Opening o(true, true, false);
        Check(o.Next(0, false) == Way::Shell, "the shell first");
        o.Sent(Way::Shell, 2.6, true);
        bool key = false;
        for (double t = 2.63; t < 12 && !o.Done(); t += kLook) {
            Way w = o.Next(t, false);
            if (w == Way::Key) key = true;
            if (w == Way::Shell) o.Sent(w, t, true);
        }
        Check(!key, "a request that took 2.6 s to be taken is never followed by the key (it would close Start on its way)");
        Check(o.Done() && !o.Opened() && o.At() >= 2.6 + kWaitAccepted - 1e-9, "...the dock waits kWaitAccepted from when the shell answered");
    }
    {   // clicks before the first request (while the dock reached the shell) are served by that request
        Opening o(true, true, false);
        o.Again(0);
        o.Again(0);
        Check(o.Next(0, false) == Way::Shell, "clicks while reaching the shell: one request goes out");
        o.Sent(Way::Shell, 0.01, true);
        Check(o.Next(0.04, false) == Way::None && o.Clicks() == 2, "...not a second one at once; the clicks are counted for the log");
    }
    {   // the button closed Start while the wait ran
        Opening o(true, true, false);
        o.Next(0, false);
        o.Sent(Way::Shell, 0.01, true);
        o.Closed(0.3);
        Check(o.Done() && o.Opened() && o.Next(1.5, false) == Way::None, "closed by the button: the wait ends, nothing more is sent");
    }

    // ---- the usual click: the shell brings Start at once
    {
        Run r = Simulate({});
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell, "usual click: the shell's request brings Start");
        Check(r.shellAsks == 1 && r.keyPresses == 0, "...one request, no Windows key");
        Check(r.o.At() < 0.25, "...and the wait ends with it");
        Check(!r.actedAfterVisible, "nothing is sent once Start is visible");
    }
    {   // the key goes first once the dock learned that (and the key reaches Start)
        Run r = Simulate({}, false);
        Check(r.o.First() == Way::Key && r.o.Opened() && r.o.CameAfter() == Way::Key && r.shellAsks == 0,
              "key-first: the Windows key brings Start, the shell is not asked");
    }
    {   // Start already up when the wait begins (the user pressed the Windows key himself meanwhile)
        World w; w.visibleAtClick = true;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.shellAsks == 0 && r.keyPresses == 0 && r.o.CameAfter() == Way::None,
              "Start already visible: nothing is sent (no toggle shut)");
    }

    // ---- the reported case: an administrator's window in front drops the injected key (UIPI)
    {
        World w; w.keyBlocked = true; w.keyReaches = false;
        Run old = Simulate(w, false);   // key-first is what mod.14 did, minus the shell
        Check(old.o.First() == Way::Shell, "a program as administrator in front: the shell goes first even when the key would");
        Check(old.o.Opened() && old.keyPresses == 0, "...Start comes and no key is pressed into the void");
    }
    {   // the key dropped silently (a hook, or elevation the dock could not see): the shell brings Start
        World w; w.keyReaches = false;
        Run r = Simulate(w, false);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell, "key dropped without a trace: the shell's request brings Start");
        Check(r.keyPresses == 1 && r.shellAsks == 1, "...one key, then one request");
        Check(r.o.At() >= kShellAfterKey && r.o.At() < kShellAfterKey + 0.3, "...after kShellAfterKey");
    }

    // ---- right after logon: Start slow to come
    {
        World w; w.shellDelay = 1.8;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell, "cold Start (1.8 s): the shell's request brings it");
        Check(r.keyPresses == 0 && r.cancelled == 0, "...no Windows key that would toggle it shut on its way");
        Check(r.shellAsks == 1, "...and no second request (one can stop the Start on its way)");
    }
    for (double slow : { 2.6, 3.0, 3.5, 5.0, 7.5 }) {   // slower still: the old key at 2.5 s closed these
        World w; w.shellDelay = slow;
        Run r = Simulate(w);
        char what[128];
        std::snprintf(what, sizeof what, "very cold Start (%.1f s after the request): it comes, no key closes it on its way", slow);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell && r.keyPresses == 0 && r.cancelled == 0, what);
    }
    {   // the old button on the same cold Start: every click toggled; now more clicks only ask again
        World w; w.shellDelay = 2.2; w.clicks = { 0.3, 0.6, 0.9, 1.2, 1.5 };
        Run r = Simulate(w);
        Check(r.o.Opened() && r.cancelled == 0 && r.keyPresses == 0, "five more clicks while a cold Start comes: it still comes, nothing toggles it shut");
        Check(r.shellAsks == 1 && r.o.Clicks() == 5, "...one request in all; the clicks are counted for the log");
    }
    {   // the live test on a test PC (2026-10-27 11:43): three clicks 80 ms apart, Start in about 70 ms
        World w; w.shellDelay = 0.07; w.clicks = { 0.08, 0.16 };
        Run r = Simulate(w);
        Check(r.o.Opened() && r.shellAsks == 1 && r.cancelled == 0 && r.keyPresses == 0,
              "three quick clicks: one request, Start comes (three requests in 0.2 s brought none on that PC)");
    }
    {   // key-first and slow: more clicks never press the key again
        World w; w.keyDelay = 0.5; w.clicks = { 0.1, 0.2 };
        Run r = Simulate(w, false);
        Check(r.keyPresses == 1 && r.cancelled == 0 && r.o.Opened(), "key-first, clicks while it comes: no second key (it would close Start)");
    }

    // ---- the shell does not help
    {
        World w; w.shellAccepts = false;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Key, "the shell refuses: the Windows key at once brings Start");
        Check(r.keyAt < 0.05, "...without waiting");
    }
    {
        World w; w.shellReady = false;
        Run r = Simulate(w);
        Check(r.o.First() == Way::Key && r.o.Opened() && r.shellAsks == 0, "no launcher (Explorer restarting): the Windows key, as before");
    }
    {
        World w; w.shellShows = false;
        Run r = Simulate(w);
        Check(!r.o.Opened() && r.keyPresses == 0, "the shell takes the request but Start never comes: no key (it could close a slow Start)");
        Check(r.o.At() >= kWaitAccepted - 1e-9 && r.o.At() < kWaitAccepted + kLook * 1.5, "...the dock waits kWaitAccepted, then logs it");
        Check(r.shellAsks == 1, "...after one request");
    }
    {
        World w; w.shellAccepts = false; w.keyBlocked = true; w.keyReaches = false;
        Run r = Simulate(w);
        Check(!r.o.Opened() && r.keyPresses == 0, "shell refuses, key blocked: no key pressed into the void");
        Check(r.shellAsks == kMaxShell, "...the shell asked kMaxShell times, a second apart");
        Check(r.end < kGiveUp, "...and the dock stops as soon as nothing is left to try");
    }
    {
        World w; w.shellAccepts = false; w.keyReaches = false;
        Run r = Simulate(w);
        Check(!r.o.Opened() && r.o.Done() && r.o.At() >= kGiveUp && r.o.At() < kGiveUp + kLook * 1.5,
              "nothing brings Start (the shell refuses, the key is dropped): the dock gives up at kGiveUp (and logs what it saw)");
        Check(r.keyPresses == 1 && r.shellAsks == 1, "...after both ways (no request after the key: it might be bringing Start)");
    }
    {
        World w; w.keyInserted = false;
        Run r = Simulate(w, false);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell && r.lastShellAt < 0.05,
              "key-first, the key not even inserted (input blocked by another thread): the shell at once");
    }
    {
        World w; w.keyInserted = false; w.shellReady = false;
        Run r = Simulate(w, false);
        Check(!r.o.Opened() && r.end < 0.1, "key not inserted and no shell: the dock stops at once");
    }

    // ---- what the dock learns
    {
        Preference p;
        World dud; dud.shellShows = false;
        Check(Learn(p, Simulate(dud, p.shellFirst).o) == Lesson::None && p.shellFirst && p.misses == 1,
              "the shell took the request and Start did not come: one miss, the shell still goes first");
        Check(Learn(p, Simulate(dud, p.shellFirst).o) == Lesson::KeyFirst && !p.shellFirst,
              "kMissesKeyFirst such clicks in a row: the key goes first from now on");
        Run next = Simulate(dud, p.shellFirst);
        Check(next.o.First() == Way::Key && next.o.Opened() && next.o.CameAfter() == Way::Key, "...and brings Start");
        World blocked; blocked.keyReaches = false;
        Run r2 = Simulate(blocked, p.shellFirst);
        Check(r2.o.Opened() && Learn(p, r2.o) == Lesson::ShellFirst && p.shellFirst && p.misses == 0,
              "the key then fails, the shell brings Start: the shell goes first again");
        World admin; admin.keyBlocked = true; admin.keyReaches = false;
        p.shellFirst = false;
        Run r3 = Simulate(admin, p.shellFirst);
        Check(r3.o.First() == Way::Shell && r3.o.Opened() && Learn(p, r3.o) == Lesson::ShellFirst,
              "key first, an administrator's window in front: the shell goes first for that click, brings Start, and first again after");
    }
    {
        Preference p;
        World w; w.shellAccepts = false;
        Run r = Simulate(w, p.shellFirst);
        Check(r.o.Opened() && Learn(p, r.o) == Lesson::None && p.misses == 1, "a refusal (the key then brought Start) is a miss");
        Check(Learn(p, Simulate(w, p.shellFirst).o) == Lesson::KeyFirst && !p.shellFirst, "two in a row: the key goes first");
        Run after = Simulate(w, p.shellFirst);
        Check(after.o.First() == Way::Key && after.o.Opened() && after.shellAsks == 0, "...and the shell is no longer bothered while the key works");
    }
    {   // Explorer not reachable for a while (after logon, restarting): passes, teaches nothing
        Preference p;
        World bad; bad.shellReady = false;
        for (int i = 0; i < 5; ++i) Check(Learn(p, Simulate(bad, p.shellFirst).o) == Lesson::None, i ? "...again" : "the shell not reachable: nothing learned");
        Check(p.shellFirst && p.misses == 0, "...five clicks later the shell still goes first (no switch-off for the rest of the run)");
        Run r = Simulate(World{}, p.shellFirst);
        Check(r.o.First() == Way::Shell && r.o.CameAfter() == Way::Shell, "...and brings Start once Explorer is there");
    }
    {
        Preference p;
        World dud; dud.shellShows = false;
        Learn(p, Simulate(dud).o);
        Learn(p, Simulate(World{}).o);
        Check(p.misses == 0 && p.shellFirst, "a miss, then Start by the shell: the misses start over");
    }

    // ---- the taskbar's own Start button (taskbar_start.h): pressed first, checked until it has brought Start
    {
        Preference p;
        Check(TaskbarUseFor(p, true) == TaskbarUse::Unproven, "a new run: the taskbar's button is pressed first, and checked");
        Check(TaskbarUseFor(p, false) == TaskbarUse::None, "...not when there is none to press (no taskbar on the monitor, not in the tree)");
        Check(!ClosesWithTaskbar(p), "...and Start is still closed with the Windows key until it has proved itself");
        p.taskbar = TaskbarState::Works;
        Check(TaskbarUseFor(p, true) == TaskbarUse::Proven && ClosesWithTaskbar(p), "once it brought Start: trusted, and it closes Start too");
        p.taskbar = TaskbarState::Failed;
        Check(TaskbarUseFor(p, true) == TaskbarUse::None && !ClosesWithTaskbar(p), "left alone after kTaskbarMissesOff misses");
        p.taskbar = TaskbarState::Off;
        Check(TaskbarUseFor(p, true) == TaskbarUse::None, "off: never pressed (the worker test's safe mode)");
    }
    {
        World w; w.taskbar = TaskbarUse::Unproven;
        Run r = Simulate(w);
        Check(r.o.First() == Way::Taskbar && r.o.Opened() && r.o.CameAfter() == Way::Taskbar,
              "the usual click: the taskbar's button brings Start, as the taskbar's own click would");
        Check(r.taskbarPresses == 1 && r.shellAsks == 0 && r.keyPresses == 0, "...one press; neither the shell nor the key");
        Preference p;
        Check(LearnTaskbar(p, r.o) == TaskbarLesson::Works && p.taskbar == TaskbarState::Works, "...and it is trusted from then on");
    }
    {   // an administrator's window in front: the press is no injected key, nothing drops it
        World w; w.taskbar = TaskbarUse::Unproven; w.keyBlocked = true; w.keyReaches = false;
        Run r = Simulate(w);
        Check(r.o.First() == Way::Taskbar && r.o.CameAfter() == Way::Taskbar && r.keyPresses == 0, "an administrator's window in front: the taskbar's button still first, and it brings Start");
    }
    {   // the press goes through, Start never comes (the button does nothing here): the shell after kTaskbarProof
        World w; w.taskbar = TaskbarUse::Unproven; w.taskbarShows = false;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell, "a press that brings nothing: the shell's request brings Start");
        Check(r.otherAt >= kTaskbarProof - 1e-9 && r.otherAt < kTaskbarProof + 2 * kLook, "...kTaskbarProof after the press, not before");
        Check(r.taskbarPresses == 1, "...the button pressed once (a second press would toggle)");
        Preference p;
        Check(LearnTaskbar(p, r.o) == TaskbarLesson::None && p.taskbar == TaskbarState::Unknown && p.taskbarMisses == 1, "...a miss: checked again next time");
        Check(LearnTaskbar(p, Simulate(w).o) == TaskbarLesson::Failed && p.taskbar == TaskbarState::Failed,
              "kTaskbarMissesOff such clicks in a row: the taskbar's button is left alone");
        Check(TaskbarUseFor(p, true) == TaskbarUse::None, "...the next click goes straight to the shell");
    }
    {   // the press does not go through (the kept button went stale, Explorer busy): the other ways at once
        World w; w.taskbar = TaskbarUse::Unproven; w.taskbarTakes = false;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell && r.otherAt <= kLook + 1e-9, "a press that did not go through: the shell at once");
        Preference p;
        Check(LearnTaskbar(p, r.o) == TaskbarLesson::None && p.taskbarMisses == 0, "...which teaches nothing about the button itself");
        World both = w; both.shellReady = false;
        Run k = Simulate(both);
        Check(k.o.Opened() && k.o.CameAfter() == Way::Key, "...and with no shell either: the Windows key, as before");
        Opening o(true, true, false, TaskbarUse::Unproven);
        o.Next(0, false);
        o.Sent(Way::Taskbar, 0.01, false);
        o.Next(0.04, true);   // Start up meanwhile (the user's own key)
        Check(o.Opened() && o.CameAfter() == Way::None, "...a Start that shows up anyway is not put down to the press that did not go through");
    }
    {   // trusted, and Start slow (right after logon): nothing else goes out, however long it takes
        for (double slow : { 1.0, 2.5, 4.0, 7.5 }) {
            World w; w.taskbar = TaskbarUse::Proven; w.taskbarDelay = slow; w.clicks = { 0.3, 0.9 };
            Run r = Simulate(w);
            char what[128];
            std::snprintf(what, sizeof what, "trusted button, Start in %.1f s, two more clicks: it comes, nothing else is sent", slow);
            Check(r.o.Opened() && r.o.CameAfter() == Way::Taskbar && r.taskbarPresses == 1 && r.shellAsks == 0 && r.keyPresses == 0 && r.cancelled == 0, what);
        }
        World w; w.taskbar = TaskbarUse::Proven; w.taskbarShows = false;
        Run r = Simulate(w);
        Check(!r.o.Opened() && r.shellAsks == 0 && r.keyPresses == 0, "trusted button, Start never comes: nothing else is sent (it could stop a slow Start)");
        Check(r.o.At() >= kWaitAccepted - 1e-9 && r.o.At() < kWaitAccepted + 2 * kLook, "...the dock waits kWaitAccepted, then logs it");
        Preference p; p.taskbar = TaskbarState::Works;
        Check(LearnTaskbar(p, r.o) == TaskbarLesson::Unproven && p.taskbar == TaskbarState::Unknown, "...and checks the button again next time");
    }
    {   // unproven and slow: the price of checking - the shell's request at kTaskbarProof may stop that Start
        World w; w.taskbar = TaskbarUse::Unproven; w.taskbarDelay = 1.5;
        Run fast = Simulate(w);
        Check(fast.o.Opened() && fast.o.CameAfter() == Way::Taskbar && fast.shellAsks == 0, "unproven, Start in 1.5 s: it comes by the button, before the check runs out");
        w.taskbarDelay = 2.5;
        Run slow = Simulate(w);
        Check(slow.shellAsks == 1 && slow.otherAt >= kTaskbarProof - 1e-9, "unproven, Start in 2.5 s: the shell is asked at kTaskbarProof (it can stop that Start)");
    }
    {   // closed with the button while the press was being checked: Start had come by the press
        Opening o(true, true, false, TaskbarUse::Unproven);
        Check(o.Next(0, false) == Way::Taskbar, "the taskbar's button first");
        o.Sent(Way::Taskbar, 0.02, true);
        Check(o.Next(0.5, false) == Way::None, "...nothing else while it is checked");
        o.Closed(0.6);
        Preference p;
        Check(o.Done() && o.CameAfter() == Way::Taskbar && LearnTaskbar(p, o) == TaskbarLesson::Works, "closed by the button meanwhile: the press brought Start");
    }
    {   // late first look (finding the button took long): the press still goes out, then the check runs from it
        Opening o(true, true, false, TaskbarUse::Unproven);
        Check(o.Next(6.0, false) == Way::Taskbar, "a late first look still presses the button");
        o.Sent(Way::Taskbar, 6.1, true);
        Check(o.Next(6.1 + kTaskbarProof - 0.05, false) == Way::None && o.Next(6.1 + kTaskbarProof, false) == Way::Shell,
              "...and the shell goes out kTaskbarProof after the press returned");
        o.Sent(Way::Shell, 8.2, true);
        Check(o.Next(8.3, false) == Way::None && o.GiveUpAt() >= 8.2 + kWaitAccepted - 1e-9, "...then the shell's rules: waits kWaitAccepted after it took the request");
    }

    // ---- right after a restart (the reviews of 2026-10-27): Start takes 3 s, Explorer answers at once. Checking
    // an unproven press stops it (the model's shell request on a Start on its way) - but nothing wrong is kept
    {
        Preference p;
        World w; w.taskbarDelay = 3.0;
        TaskbarLesson last = TaskbarLesson::None;
        for (int click = 0; click < 2; ++click) {
            w.taskbar = TaskbarUseFor(p, true);
            Run r = Simulate(w, p.shellFirst);
            last = LearnTaskbar(p, r.o);
            Learn(p, r.o);
        }
        Check(last == TaskbarLesson::GaveUp && p.taskbarMisses == 0 && p.shellFirst && p.misses == 0,
              "two such clicks after a restart: the button left alone for this run only (nothing kept), the shell not demoted");
        w.taskbar = TaskbarUseFor(p, true);
        w.taskbarDelay = 0.1;
        Run rest = Simulate(w, p.shellFirst);
        Check(w.taskbar == TaskbarUse::None && rest.o.Opened() && rest.o.CameAfter() == Way::Shell, "...the rest of the run the shell brings Start, as before mod.18");
        Preference next;   // the next run: nothing was kept, the button is checked again
        w.taskbar = TaskbarUseFor(next, true);
        Run warm = Simulate(w, next.shellFirst);
        Check(warm.o.CameAfter() == Way::Taskbar && LearnTaskbar(next, warm.o) == TaskbarLesson::Works, "...the next run, Start warm, proves the button");
        w.taskbar = TaskbarUseFor(next, true); w.taskbarDelay = 3.0;   // the next restart: the kept verdict trusts it
        Run cold = Simulate(w, next.shellFirst);
        Check(cold.o.Opened() && cold.o.CameAfter() == Way::Taskbar && cold.shellAsks == 0 && cold.keyPresses == 0,
              "...after which a cold Start of 3 s comes by the trusted button, nothing else sent");
    }
    {   // a cold Start that merely came after the check's shell request (which did not stop it) proves nothing
        World w; w.taskbar = TaskbarUse::Unproven; w.taskbarDelay = 3.0; w.shellStopsPending = false;
        Preference p;
        for (int click = 0; click < 3; ++click) {
            Run r = Simulate(w, p.shellFirst);
            Check(r.o.Opened() && r.o.CameAfter() == Way::Shell && LearnTaskbar(p, r.o) == TaskbarLesson::None,
                  click ? "...again" : "cold Start 3 s after the press, the shell asked at 2 s: put down to the shell, yet no miss for the button");
        }
        Check(p.taskbar == TaskbarState::Unknown && p.taskbarMisses == 0, "...three such clicks: the button is still checked, never given up");
        w.taskbarShows = false; w.shellDelay = 0.1;   // the press really does nothing, the shell brings a warm Start
        Run r = Simulate(w, p.shellFirst);
        Check(LearnTaskbar(p, r.o) == TaskbarLesson::None && p.taskbarMisses == 1, "a warm Start right after the shell's request: that is a miss");
    }
    {   // the press answers but does nothing, and the shell takes requests and brings nothing either (review 2): the key
        // is still found out, as before mod.18
        World w; w.taskbarShows = false; w.shellShows = false;
        Preference p;
        int click = 0;
        bool opened = false;
        for (; click < 8 && !opened; ++click) {
            w.taskbar = TaskbarUseFor(p, true);
            Run r = Simulate(w, p.shellFirst);
            opened = r.o.Opened();
            LearnTaskbar(p, r.o);
            Learn(p, r.o);
        }
        Check(opened && !p.shellFirst && p.taskbar == TaskbarState::Failed, "press and shell both dead: the Windows key brings Start in the end (button left alone, key first)");
        Preference q;
        World dead; dead.taskbar = TaskbarUse::Unproven; dead.taskbarShows = false; dead.shellShows = false;   // nothing comes
        World byShell; byShell.taskbar = TaskbarUse::Unproven; byShell.taskbarShows = false;                   // the shell brings it
        LearnTaskbar(q, Simulate(dead).o);
        LearnTaskbar(q, Simulate(byShell).o);
        Check(LearnTaskbar(q, Simulate(dead).o) == TaskbarLesson::None && q.taskbar == TaskbarState::Unknown,
              "a click that brought Start in between: the streak of clicks that brought nothing starts over");
        std::printf("  (press and shell both dead: Start on click %d; without the button it came on click 3)\n", click);
    }
    {   // Explorer busy (a single call into it took kBusyCall): the press is waited on alone
        World w; w.taskbar = TaskbarUse::Unproven; w.explorerBusy = true; w.taskbarDelay = 5.0;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Taskbar && r.shellAsks == 0 && r.keyPresses == 0,
              "Explorer busy: an unproven press whose Start takes 5 s still brings it - no check runs out on it");
        Preference p;
        Check(LearnTaskbar(p, r.o) == TaskbarLesson::Works, "...and proves the button");
        w.taskbarShows = false;
        Run dud = Simulate(w);
        Check(!dud.o.Opened() && dud.shellAsks == 0 && dud.o.At() >= kWaitAccepted - 1e-9, "Explorer busy, the press brings nothing: kWaitAccepted, then given up (nothing else sent)");
        Preference q;
        Check(LearnTaskbar(q, dud.o) == TaskbarLesson::None && q.taskbarMisses == 0, "...no miss either: Explorer was busy, Start may have needed longer");
    }
    {   // the press timed out - and reaches Explorer all the same, late
        World w; w.taskbar = TaskbarUse::Unproven; w.taskbarUnsure = true; w.taskbarDelay = 1.0;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Taskbar && r.shellAsks == 0 && r.keyPresses == 0 && r.cancelled == 0,
              "a press that timed out: nothing else goes out (the shell or the key on top of a late press would toggle twice)");
        Preference p;
        Check(LearnTaskbar(p, r.o) == TaskbarLesson::None && p.taskbar == TaskbarState::Unknown, "...and nothing is learned from it");
        w.taskbarShows = false;
        Run dud = Simulate(w);
        Check(!dud.o.Opened() && dud.shellAsks == 0 && dud.keyPresses == 0, "a press that timed out and never arrives: nothing else sent");
        Preference q;
        Check(LearnTaskbar(q, dud.o) == TaskbarLesson::None && q.taskbarMisses == 0, "...no miss either");
    }
    {   // the shell, reached only when needed, cannot be had behind the button: as without a shell
        World w; w.taskbar = TaskbarUse::Unproven; w.taskbarTakes = false; w.launcherFails = true;
        Run r = Simulate(w);
        Check(r.shellLost && r.shellAsks == 0 && r.o.Opened() && r.o.CameAfter() == Way::Key && r.keyAt <= kLook + 1e-9,
              "the press did not go through and the shell cannot be had: the Windows key at once");
        Preference p;
        Check(Learn(p, r.o) == Lesson::None && p.misses == 0, "...and the shell is not blamed: it was never asked");
    }
    {   // the verdict kept across runs
        const std::wstring build = L"26200.6899";
        Check(FormatVerdict(TaskbarState::Works, build, 154000) == L"works 26200.6899 154000", "the verdict is written as \"works <build> <day>\"");
        Check(FormatVerdict(TaskbarState::Unknown, build, 154000).empty() && FormatVerdict(TaskbarState::Off, build, 154000).empty(),
              "...nothing is kept for a button not proven either way");
        Check(ParseVerdict(L"works 26200.6899 154000", build, 154900) == TaskbarState::Works, "a kept \"works\" holds on the same build, however old");
        Check(ParseVerdict(L"works 26200.6899 154000", L"26200.7000", 154000) == TaskbarState::Unknown, "...a Windows update is checked afresh");
        Check(ParseVerdict(L"failed 26200.6899 154000", build, 154000 + kVerdictDays - 1) == TaskbarState::Failed, "a kept failure holds for kVerdictDays");
        Check(ParseVerdict(L"failed 26200.6899 154000", build, 154000 + kVerdictDays) == TaskbarState::Unknown, "...then the button is tried again");
        Check(ParseVerdict(L"failed 26200.6899 154000", build, 153990) == TaskbarState::Unknown, "...and at once should the clock have gone back");
        bool junk = true;
        for (const wchar_t* t : { L"", L"works", L"works 26200.6899", L"works 26200.6899 ", L"works 26200.6899 12x", L"maybe 26200.6899 154000",
                                  L"works  26200.6899 154000", L"works 26200.6899 99999999999999" })
            junk = junk && ParseVerdict(t, build, 154000) == TaskbarState::Unknown;
        Check(junk, "...anything malformed counts as nothing kept");
        Check(ParseVerdict(FormatVerdict(TaskbarState::Failed, build, 154000), build, 154001) == TaskbarState::Failed, "...and what is written reads back");
        // a build that only starts or ends the same is another build; no build known: nothing kept
        Check(ParseVerdict(L"works 26200.6899 154000", L"26200.689", 154000) == TaskbarState::Unknown &&
              ParseVerdict(L"works 26200.6899 154000", L"26200.68990", 154000) == TaskbarState::Unknown &&
              ParseVerdict(L"works 26200.6899 154000", L"", 154000) == TaskbarState::Unknown, "...a build read only in part is another build");
        Check(ParseVerdict(L"failed 26200.6899 0154000", build, 154001) == TaskbarState::Failed && ParseVerdict(L"works 26200.6899 0", build, 154000) == TaskbarState::Works,
              "...leading zeros and day 0 read as numbers");
        Check(ParseVerdict(L"failed 26200.6899 1000000009", build, 154000) == TaskbarState::Unknown &&
              ParseVerdict(L"failed 26200.6899 2147483648", build, 154000) == TaskbarState::Unknown &&
              ParseVerdict(L"failed 26200.6899 99999999999999999999", build, 154000) == TaskbarState::Unknown,
              "...a day too large is nothing kept, never an overflow");
        Check(ParseVerdict(L"Works 26200.6899 154000", build, 154000) == TaskbarState::Unknown && ParseVerdict(L"worksx 26200.6899 154000", build, 154000) == TaskbarState::Unknown &&
              ParseVerdict(L" works 26200.6899 154000", build, 154000) == TaskbarState::Unknown, "...the word must be exactly \"works\" or \"failed\"");
        long kept = -1;
        Check(ParseVerdict(L"failed 26200.6899 154000", build, 154001, &kept) == TaskbarState::Failed && kept == 154000, "a failure read back tells the day it was kept");
        kept = -1;
        Check(ParseVerdict(L"works 26200.6899 154000", build, 154001, &kept) == TaskbarState::Works && kept == -1 &&
              ParseVerdict(L"failed 26200.6899 154000", build, 154000 + kVerdictDays, &kept) == TaskbarState::Unknown && kept == -1, "...nothing else does");
    }
    {   // within a run that lasts days (a laptop sleeps, it does not restart): a kept failure runs out the same way
        Check(!VerdictExpired(154000, 154000) && !VerdictExpired(154000, 154000 + kVerdictDays - 1) && VerdictExpired(154000, 154000 + kVerdictDays) &&
              VerdictExpired(154000, 153999), "a kept failure is over kVerdictDays later, or at once should the clock have gone back");
        bool same = true;
        for (long when = 153990; when <= 154010; ++when)
            for (long day = 153980; day <= 154020; ++day)
                same = same && (ParseVerdict(FormatVerdict(TaskbarState::Failed, L"26200.6899", when), L"26200.6899", day) == TaskbarState::Failed) == !VerdictExpired(when, day);
        Check(same, "...the same rule for a failure read back at a run's first click and one within the run");
        Preference p;
        p.taskbar = TaskbarState::Failed; p.taskbarFailedDay = 154000; p.taskbarMisses = kTaskbarMissesOff; p.unclear = 1;
        Check(!RetryTaskbar(p, 154000 + kVerdictDays - 1) && p.taskbar == TaskbarState::Failed && TaskbarUseFor(p, true) == TaskbarUse::None,
              "a failure kept in this run: left alone for kVerdictDays");
        Check(RetryTaskbar(p, 154000 + kVerdictDays) && p.taskbar == TaskbarState::Unknown && p.taskbarMisses == 0 && p.unclear == 0 && p.taskbarFailedDay == -1 &&
              TaskbarUseFor(p, true) == TaskbarUse::Unproven, "...then pressed and checked again, from no misses, as the next run would");
        Check(!RetryTaskbar(p, 154000 + 10 * kVerdictDays) && p.taskbar == TaskbarState::Unknown, "...once");
        Preference back;
        back.taskbar = TaskbarState::Failed; back.taskbarFailedDay = 154000;
        Check(RetryTaskbar(back, 153999) && back.taskbar == TaskbarState::Unknown, "...at once should the clock have gone back");
        Preference run;
        run.taskbar = TaskbarState::Failed;   // GaveUp, or learned in Windows' or Explorer's first minutes: never kept
        Check(!RetryTaskbar(run, 1000000) && run.taskbar == TaskbarState::Failed, "a failure for this run only stays for the run");
        bool untouched = true;
        for (TaskbarState s : { TaskbarState::Unknown, TaskbarState::Works, TaskbarState::Off }) {
            Preference q;
            q.taskbar = s; q.taskbarFailedDay = 154000; q.taskbarMisses = 1;
            untouched = untouched && !RetryTaskbar(q, 160000) && q.taskbar == s && q.taskbarMisses == 1;
        }
        Check(untouched, "...and a button not left alone is not touched");
    }
    {   // what is learned right after Windows or Explorer started is not kept
        Check(EarlyForVerdict(kVerdictUptime - 1, -1) && !EarlyForVerdict(kVerdictUptime, -1), "a failure learned in Windows' first kVerdictUptime is not kept");
        Check(EarlyForVerdict(864000, 60) && EarlyForVerdict(864000, kVerdictUptime - 0.1) && !EarlyForVerdict(864000, kVerdictUptime),
              "...nor in Explorer's, Windows up for days (a logon after fast startup, or Explorer restarted)");
        Check(!EarlyForVerdict(864000, -1) && EarlyForVerdict(60, 864000), "...Explorer's age not to be had: Windows' uptime alone, as before; either young is enough");
    }

    // ---- every combination: the rules that must always hold
    portable_check::Quiet() = true;
    int combos = 0;
    const double delays[] = { 0.05, 0.4, 0.9, 1.6, 2.2, 2.6, 3.0, 3.5, 5.0, 7.5 };
    const std::vector<std::vector<double>> clickSets = { {}, { 0.15 }, { 0.2, 0.5, 0.8, 1.1, 1.4, 2.6, 3.0 } };
    for (int bits = 0; bits < 128; ++bits)
        for (double delay : delays)
            for (const auto& clicks : clickSets) {
                World w;
                w.shellReady = bits & 1; w.shellAccepts = bits & 2; w.shellShows = bits & 4;
                w.keyInserted = bits & 8; w.keyReaches = bits & 16; w.keyBlocked = bits & 32;
                bool shellFirst = bits & 64;
                w.shellDelay = delay; w.clicks = clicks;
                Run r = Simulate(w, shellFirst);
                ++combos;
                std::string d = Describe(w) + (shellFirst ? " shell-first" : " key-first");
                const double giveUp = r.firstAcceptAt >= 0 ? std::max(kGiveUp, r.firstAcceptAt + kWaitAccepted) : kGiveUp;
                Check(r.o.Done() && r.end <= giveUp + kLook, ("always ends by kGiveUp, or kWaitAccepted after the shell took a request: " + d).c_str());
                Check(r.keyPresses <= 1, ("the key at most once per click: " + d).c_str());
                Check(!r.keyAfterAccept, ("never the key after the shell took a request: " + d).c_str());
                Check(!r.sentAfterAccept, ("nothing at all after the shell took a request: " + d).c_str());
                Check(r.shellAsks <= kMaxShell, ("the shell at most kMaxShell times: " + d).c_str());
                Check(!r.actedAfterVisible, ("nothing sent once Start is visible: " + d).c_str());
                Check(!(w.keyBlocked && w.shellReady && r.keyPresses), ("no key into an administrator's window when the shell can be asked: " + d).c_str());
                Check(!r.shellTooSoon, ("a refused request repeated only after kShellAgain, clicks or not: " + d).c_str());
                Check(r.cancelled == 0, ("nothing the dock sends stops a Start on its way: " + d).c_str());
                bool shellBrings = w.shellReady && w.shellAccepts && w.shellShows;
                bool keyBrings = w.keyInserted && w.keyReaches && !w.keyBlocked;
                if (shellBrings && delay < kWaitAccepted - 0.1 && (shellFirst || w.keyBlocked))
                    Check(r.o.Opened() && r.cancelled == 0 && r.keyPresses == 0 && r.o.CameAfter() == Way::Shell,
                          ("shell first: a Start the shell brings, however slowly, is never toggled shut, and no key is pressed: " + d).c_str());
                if (shellBrings && delay < kWaitAccepted - kShellAfterKey - 0.1 && !shellFirst && !keyBrings)
                    Check(r.o.Opened() && r.o.CameAfter() == Way::Shell,
                          ("key first, the key does not reach Start: the shell brings it: " + d).c_str());
                const bool shellTakes = w.shellReady && w.shellAccepts && (shellFirst || w.keyBlocked);
                if (!shellBrings && keyBrings && !shellTakes)
                    Check(r.o.Opened() && r.o.CameAfter() == Way::Key, ("the key brings Start when the shell cannot: " + d).c_str());
                if (shellTakes && !w.shellShows)
                    Check(!r.o.Opened() && r.keyPresses == 0, ("the shell takes the request, Start never comes: no key, not opened (learned after kMissesKeyFirst): " + d).c_str());
                if (r.o.Opened()) Check(r.o.CameAfter() != Way::None || w.visibleAtClick, ("Start's coming is put down to a way: " + d).c_str());
                if (!shellBrings && !keyBrings) Check(!r.o.Opened(), ("nothing can bring Start: not reported as opened: " + d).c_str());
            }
    // the same with the taskbar's button pressed first - checked or trusted, going through, not, or perhaps
    // (a timeout), bringing Start fast, slowly or never, Explorer busy or not, the shell reachable behind it or
    // not - and its own rules, what is learned included
    const double taskbarDelays[] = { 0.05, 0.9, 1.9, 2.6, 5.0, 7.5 };
    const double shellDelays[] = { 0.05, 1.6, 3.0, 7.5 };
    for (int bits = 0; bits < 128; ++bits)
        for (int tb = 0; tb < 128; ++tb)
            for (double tdelay : taskbarDelays)
                for (double sdelay : shellDelays)
                    for (const auto& clicks : clickSets) {
                        World w;
                        w.shellReady = bits & 1; w.shellAccepts = bits & 2; w.shellShows = bits & 4;
                        w.keyInserted = bits & 8; w.keyReaches = bits & 16; w.keyBlocked = bits & 32;
                        const bool shellFirst = bits & 64;
                        w.taskbar = (tb & 1) ? TaskbarUse::Proven : TaskbarUse::Unproven;
                        w.taskbarTakes = tb & 2; w.taskbarShows = tb & 4; w.taskbarUnsure = tb & 8;
                        w.explorerBusy = tb & 16; w.launcherFails = tb & 32; w.shellStopsPending = !(tb & 64);
                        w.taskbarDelay = tdelay; w.shellDelay = sdelay; w.clicks = clicks;
                        Run r = Simulate(w, shellFirst);
                        ++combos;
                        const std::string d = Describe(w) + (shellFirst ? " shell-first" : " key-first");
                        const bool alone = WaitsAlone(w);
                        const bool arrives = w.taskbarTakes || w.taskbarUnsure;
                        double bound;
                        if (r.taskbarTook && alone) bound = r.taskbarAt + kWaitAccepted;
                        else {
                            const double from = r.otherAt >= 0 ? r.otherAt : 0;
                            bound = from + kGiveUp;
                            if (r.firstAcceptAt >= 0) bound = std::max(bound, r.firstAcceptAt + kWaitAccepted);
                        }
                        Check(r.o.Done() && r.end <= bound + kLook, ("taskbar first: always ends (kWaitAccepted after a press waited on alone, else as the other ways do): " + d).c_str());
                        Check(r.taskbarPresses == 1, ("the taskbar's button exactly once per click (a second press would toggle): " + d).c_str());
                        Check(r.firstWay == Way::Taskbar, ("the taskbar's button is what goes out first, whatever else holds: " + d).c_str());
                        Check(!r.sentAfterTrusted, ("nothing after a press waited on alone (trusted, Explorer busy, maybe on its way): " + d).c_str());
                        Check(!r.otherTooSoon, ("neither the shell nor the key within kTaskbarProof of a press that went through: " + d).c_str());
                        Check(r.keyPresses <= 1 && r.shellAsks <= kMaxShell, ("the key at most once, the shell at most kMaxShell times: " + d).c_str());
                        Check(!r.keyAfterAccept && !r.sentAfterAccept, ("nothing after the shell took a request: " + d).c_str());
                        Check(!r.actedAfterVisible, ("nothing sent once Start is visible: " + d).c_str());
                        Check(!r.shellTooSoon, ("a refused request repeated only after kShellAgain: " + d).c_str());
                        if (arrives && w.taskbarShows && tdelay < (alone ? kWaitAccepted : kTaskbarProof) - 0.1)
                            Check(r.o.Opened() && r.o.CameAfter() == Way::Taskbar && r.cancelled == 0 && r.shellAsks == 0 && r.keyPresses == 0,
                                  ("a Start the taskbar's button brings in time comes by it alone, never stopped: " + d).c_str());
                        if (r.cancelled)
                            Check(!alone && w.taskbarTakes && w.taskbarShows && tdelay >= kTaskbarProof - 1e-9,
                                  ("a Start on its way is only ever stopped by checking an unproven press slower than kTaskbarProof, Explorer not busy: " + d).c_str());
                        if (arrives && !w.taskbarShows && alone)
                            Check(!r.o.Opened() && r.shellAsks == 0 && r.keyPresses == 0, ("a press waited on alone that brings nothing: nothing else is sent, not opened: " + d).c_str());
                        if (!arrives || (!w.taskbarShows && !alone)) {   // as if there were no button: the same ways, the same outcome
                            World plain = w;
                            plain.taskbar = TaskbarUse::None;
                            plain.shellReady = w.shellReady && !w.launcherFails;   // a shell that could not be had, had there been no button
                            Run q = Simulate(plain, shellFirst);
                            Check(r.o.Opened() == q.o.Opened() && r.o.CameAfter() == q.o.CameAfter() && r.shellAsks == q.shellAsks && r.keyPresses == q.keyPresses,
                                  ("a press that did not go through, or brought nothing unproven: the click goes on as without the button: " + d).c_str());
                        }
                        // what is learned
                        Preference p;
                        p.taskbar = w.taskbar == TaskbarUse::Proven ? TaskbarState::Works : TaskbarState::Unknown;
                        const TaskbarState before = p.taskbar;
                        const TaskbarLesson lt = LearnTaskbar(p, r.o);
                        const bool noTelling = r.taskbarTook && (r.shellAsks || r.keyPresses) && !r.o.Opened();
                        if (w.taskbarUnsure)
                            Check(lt == TaskbarLesson::None && p.taskbarMisses == 0 && p.taskbar == before, ("nothing learned from a press that may or may not have arrived: " + d).c_str());
                        else if (noTelling)
                            Check(lt == TaskbarLesson::None && p.taskbarMisses == 0, ("other ways went out and nothing came: the button is not blamed (no telling): " + d).c_str());
                        if (r.o.Opened() && r.o.CameAfter() == Way::Taskbar && !w.taskbarUnsure)
                            Check(p.taskbar == TaskbarState::Works && p.taskbarMisses == 0, ("Start by the button: it works (trusted from then on): " + d).c_str());
                        if (w.taskbarTakes && !w.taskbarUnsure && !w.explorerBusy && !r.o.Opened() && !r.shellAsks && !r.keyPresses)
                            Check(p.taskbarMisses == 1, ("a press waited on alone, Explorer not busy, that brought nothing in kWaitAccepted: a miss: " + d).c_str());
                        {   // a miss only on evidence
                            const bool warmByOther = r.o.Opened() && (r.o.CameAfter() == Way::Shell || r.o.CameAfter() == Way::Key) &&
                                                     r.o.OtherAt() >= 0 && r.o.At() - r.o.OtherAt() <= kShellAfterKey + 1e-9;
                            const bool lone = !r.o.Opened() && !r.shellAsks && !r.keyPresses && !w.explorerBusy && !w.taskbarUnsure;
                            if (p.taskbarMisses > 0 || lt == TaskbarLesson::Failed || lt == TaskbarLesson::Unproven)
                                Check(warmByOther || lone, ("the button gets a miss only on evidence (a warm Start by another way, or a clean lone wait): " + d).c_str());
                            Check(lt != TaskbarLesson::GaveUp, ("one click alone never gives the button up: " + d).c_str());
                        }
                        Preference q;
                        const Lesson ls = Learn(q, r.o);
                        if (r.taskbarTook && !(r.o.Opened() && r.o.CameAfter() == Way::Shell))
                            Check(ls == Lesson::None && q.misses == 0, ("the shell is not blamed for a click behind a press that went through: " + d).c_str());
                        if (r.shellLost) Check(!r.shellAsks && ls == Lesson::None, ("a shell that could not be had is not asked, nor blamed: " + d).c_str());
                    }
    portable_check::Quiet() = false;
    std::printf("checked %d combinations\n", combos);
    return Summary("start_menu_test");
}
