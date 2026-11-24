// The dock's start button (src/start_menu_logic.h): a click goes down a ladder - ask the shell for Start once
// and watch whether it comes; kConfirm after the shell took the request, press the taskbar's own Start button,
// and only when its toggle state reads off; kGiveUp after the shell took it, the Windows key - each way at most
// once. A way that was surely not taken (the shell not to be had or refusing, no button, a press that did not go
// through) has nothing on its way: the next one at once. Slow (minutes after Windows or Explorer started, or a
// call into Explorer that took kBusyCall), whatever was taken is waited on alone; so is a press that timed out.
// Clicks made while waiting are counted, nothing is sent for them. Nothing is remembered between clicks.
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

// The taskbar button's toggle state as the worker reads it right before a press - what was not measured when the
// ladder was written (does the button follow a Start the shell opened?), so every world is run each way.
enum class Toggle { Follows,    // on while a Start is on its way or up, whoever brought it; off otherwise
                    StaysOff,   // off whatever the shell brought: a press onto a Start on its way stops it
                    StuckOn };  // on with Start closed (out of step, as suspected on 2026-11-17): never pressed

// How the simulated Windows answers.
struct World {
    bool shellReady = true;     // the launcher could be had
    bool shellAccepts = true;   // ShowStartView returns S_OK
    bool shellShows = true;     // ... and Start then comes
    double shellDelay = 0.12;   // after the accepted request
    bool keyInserted = true;    // SendInput inserted both events
    bool keyReaches = true;     // not dropped on the way (UIPI, a keyboard hook)
    bool keyBlocked = false;    // the dock sees a program that runs as administrator in front
    double keyDelay = 0.08;
    bool button = true;         // a Start button is to be had for the dock's monitor (looked for at its turn)
    Toggle toggle = Toggle::Follows;
    bool taskbarTakes = true;   // the press goes through (its pattern answers S_OK)
    bool taskbarUnsure = false; // ...or it times out - and reaches Explorer all the same, late (taskbarTakes ignored)
    bool taskbarShows = true;   // Start then comes
    double taskbarDelay = 0.1;
    int slow = 0;               // 1: slow from the start (minutes after Windows or Explorer started, or reaching the shell
                                // was slow); 2: the press call itself took kBusyCall (the worker calls Slow() after it)
    bool visibleAtClick = false;
    std::vector<double> clicks; // more clicks while waiting (seconds after the first)
};

struct Run {
    Opening o;
    int shellAsks = 0, keyPresses = 0, taskbarPresses = 0, cancelled = 0;
    double acceptAt = -1, shellAt = -1, pressAt = -1, keyAt = -1, turnAt = -1;
    double takenAt = -1, sentAt = -1;  // the last thing taken (accepted, a press that arrives, a key inserted); the last thing sent
    Way firstWay = Way::None;          // what went out first
    bool pressArrives = false;         // a press went through (or may have)
    bool heldOn = false;               // the button's turn came, its state read on: not pressed
    double startAt = -1;               // when Start shows (-1: not coming)
    double pendingDelay = 0;           // how long the Start now on its way takes, from what brought it
    Way pendingBy = Way::None;         // ...and what brought it
    double cancelledDelay = -1;        // the Start the dock stopped: how slow it was...
    Way cancelledBy = Way::None, cancelledOf = Way::None;   // ...what stopped it, and whose it was
    bool cancelledSlow = false;        // ...and whether the click was slow then
    bool tooSoonAfterAccept = false;   // anything within kConfirm of a request the shell took
    bool sentAfterTakenSlow = false;   // anything after something was taken, the click being slow
    bool keyTooSoon = false;           // the key within kGiveUp of a request the shell took, or within kConfirm of a press that went through
    bool sentAfterUnsure = false;      // anything after a press that may still arrive
    bool outOfOrder = false;           // the shell behind another way, or the button behind the key
    bool actedAfterVisible = false;
    double end = 0;
    explicit Run(const World& w) : o(w.shellReady, w.keyBlocked) {}
};

bool StateOn(const World& w, const Run& r) {
    if (w.toggle == Toggle::StuckOn) return true;
    return w.toggle == Toggle::Follows && r.startAt >= 0;   // (asked only while Start is not visible: on its way)
}

Run Simulate(const World& w) {
    Run r(w);
    if (w.visibleAtClick) r.startAt = 0;
    if (w.slow == 1) r.o.Slow();
    size_t nextClick = 0;
    for (int i = 0; i < 1000 && !r.o.Done(); ++i) {
        double t = i * kLook;
        bool visible = r.startAt >= 0 && t >= r.startAt;
        while (nextClick < w.clicks.size() && w.clicks[nextClick] <= t) { r.o.Again(t); ++nextClick; }
        Way way = r.o.Next(t, visible);
        // the button's turn: the worker looks for it now and reads its state right before the press - none to be
        // had, or not off, and nothing is pressed: it tells the opening and asks it again at once
        if (way == Way::Taskbar) {
            if (r.turnAt < 0) r.turnAt = t;
            const bool on = w.button && StateOn(w, r);
            if (!w.button || on) {
                r.heldOn = on;
                r.o.NoPress(t);
                way = r.o.Next(t, visible);
                if (way == Way::Taskbar) r.outOfOrder = true;   // (asked for the button twice)
            }
        }
        if (way != Way::None) {
            if (r.firstWay == Way::None) r.firstWay = way;
            if (visible) r.actedAfterVisible = true;
            if (r.acceptAt >= 0 && t - r.acceptAt < kConfirm - 1e-9) r.tooSoonAfterAccept = true;
            if (r.o.IsSlow() && (r.acceptAt >= 0 || r.pressArrives)) r.sentAfterTakenSlow = true;
            if (r.pressArrives && w.taskbarUnsure) r.sentAfterUnsure = true;
            if (way == Way::Key && r.acceptAt >= 0 && t - r.acceptAt < kGiveUp - 1e-9) r.keyTooSoon = true;
            if (way == Way::Key && r.pressArrives && t - r.pressAt < kConfirm - 1e-9) r.keyTooSoon = true;
            if (way == Way::Shell && (r.taskbarPresses || r.keyPresses || r.turnAt >= 0)) r.outOfOrder = true;
            if (way == Way::Taskbar && r.keyPresses) r.outOfOrder = true;
            r.sentAt = t;
        }
        const bool pending = r.startAt >= 0 && t < r.startAt;
        auto stop = [&](Way by) {   // a toggle onto a Start on its way: it is stopped, and none comes of it
            r.cancelledDelay = r.pendingDelay; r.cancelledOf = r.pendingBy; r.cancelledBy = by; r.cancelledSlow = r.o.IsSlow();
            r.startAt = -1; ++r.cancelled;
        };
        if (way == Way::Taskbar) {
            ++r.taskbarPresses; r.pressAt = t;
            const bool arrives = w.taskbarTakes || w.taskbarUnsure;
            if (arrives) {   // like a click on the taskbar's button: it toggles
                r.pressArrives = true;
                r.takenAt = t;
                if (pending) stop(Way::Taskbar);
                else if (w.taskbarShows && r.startAt < 0 && r.cancelled == 0) { r.startAt = t + w.taskbarDelay; r.pendingDelay = w.taskbarDelay; r.pendingBy = Way::Taskbar; }
            }
            r.o.Sent(Way::Taskbar, t, arrives);
            if (w.taskbarUnsure) r.o.Uncertain();
            else if (w.slow == 2) r.o.Slow();   // the press call itself was slow: Explorer is busy
        } else if (way == Way::Shell) {
            ++r.shellAsks; r.shellAt = t;
            const bool ok = w.shellAccepts;
            if (ok && w.shellShows && r.startAt < 0) { r.startAt = t + w.shellDelay; r.pendingDelay = w.shellDelay; r.pendingBy = Way::Shell; }
            if (ok) { r.acceptAt = t; r.takenAt = t; }
            r.o.Sent(Way::Shell, t, ok);
        } else if (way == Way::Key) {
            ++r.keyPresses; r.keyAt = t;
            if (w.keyInserted) r.takenAt = t;
            if (w.keyInserted && w.keyReaches && !w.keyBlocked) {
                if (pending) stop(Way::Key);   // toggled shut on its way
                else if (r.startAt < 0) { r.startAt = t + w.keyDelay; r.pendingDelay = w.keyDelay; r.pendingBy = Way::Key; }
            }
            r.o.Sent(Way::Key, t, w.keyInserted);
        }
        r.end = t;
    }
    return r;
}

std::string Describe(const World& w) {
    char b[400];
    std::snprintf(b, sizeof b, "ready=%d accepts=%d shows=%d delay=%.2f inserted=%d reaches=%d blocked=%d kdelay=%.2f clicks=%zu button=%d toggle=%s takes=%d unsure=%d shows=%d delay=%.2f slow=%d",
                  w.shellReady, w.shellAccepts, w.shellShows, w.shellDelay, w.keyInserted, w.keyReaches, w.keyBlocked, w.keyDelay, w.clicks.size(), w.button,
                  w.toggle == Toggle::Follows ? "follows" : w.toggle == Toggle::StaysOff ? "off" : "on",
                  w.taskbarTakes, w.taskbarUnsure, w.taskbarShows, w.taskbarDelay, w.slow);
    return b;
}

bool Near(double t, double at) { return t >= at - 1e-9 && t < at + 2 * kLook; }   // at the look that reached `at`

}   // namespace

int main() {
    // ---- the taskbar button's toggle state in dock.log (UI Automation's ToggleState; -1: not read)
    Check(std::wstring(ToggleText(0)) == L"off" && std::wstring(ToggleText(1)) == L"on" && std::wstring(ToggleText(2)) == L"indeterminate",
          "the toggle state reads off, on, indeterminate in dock.log");
    Check(std::wstring(ToggleText(-1)) == L"?" && std::wstring(ToggleText(3)) == L"?", "...and ? when not read, or a value UI Automation never gives");

    // ---- what a press means
    Check(OnPress(false, false) == Press::Open, "Start not in front: the button opens it");
    Check(OnPress(true, false) == Press::Close, "Start in front: the button closes it, as the taskbar's does");
    Check(OnPress(false, true) == Press::Again, "a click while Start is still coming joins that wait (no second toggle)");
    Check(OnPress(true, true) == Press::Close, "Start in front closes it even while a wait still runs (the wait is stopped)");
    Check(StillOpening(true, 0.5) && StillOpening(true, kWaitAccepted + 1), "a running worker is waited on (it gives up by itself)");
    Check(!StillOpening(true, kStuck + 0.1), "a worker stuck on a hung Explorer is left behind: the next click starts over");
    Check(!StillOpening(false, 0.1), "no worker: a click opens");
    Check(kStuck > kWaitAccepted + 3, "kStuck leaves a slow but working wait alone (and a few seconds to reach the shell)");
    Check(kConfirm < kGiveUp && kGiveUp + kConfirm < kWaitAccepted, "the ladder's times: the button at kConfirm, the key at kGiveUp, and time left for the key before kWaitAccepted");

    // ---- the timeline: the first request always goes out; waits run from when a call returned
    {
        Opening o(true, false);
        Check(o.Next(5.0, false) == Way::Shell, "a first look after kGiveUp (reaching a busy Explorer took long) still asks the shell");
        o.Sent(Way::Shell, 5.2, true);
        Check(o.Next(5.25, false) == Way::None && !o.Done(), "...and then waits for Start instead of giving up at once");
        Check(o.Next(5.2 + kConfirm - 0.05, false) == Way::None && o.Next(5.2 + kConfirm, false) == Way::Taskbar,
              "...the taskbar's button has its turn kConfirm after the shell answered, not after the click");
        o.NoPress(5.2 + kConfirm);
        Check(o.Next(5.2 + kGiveUp - 0.05, false) == Way::None && o.Next(5.2 + kGiveUp, false) == Way::Key, "...and the key kGiveUp after the shell answered");
        o.Sent(Way::Key, 5.2 + kGiveUp, true);
        Check(o.Next(5.2 + kWaitAccepted - 0.05, false) == Way::None && !o.Done(), "...the wait goes on to kWaitAccepted after the shell answered");
        o.Next(5.2 + kWaitAccepted, false);
        Check(o.Done() && !o.Opened() && o.At() >= 5.2 + kWaitAccepted - 1e-9, "...for kWaitAccepted after the shell took the request");
    }
    {   // ShowStartView itself took 2.6 s (a busy Explorer: the worker calls Slow) and returned S_OK: nothing at any time after it
        Opening o(true, false);
        Check(o.Next(0, false) == Way::Shell, "the shell first");
        o.Sent(Way::Shell, 2.6, true);
        o.Slow();
        bool sent = false;
        for (double t = 2.63; t < 12 && !o.Done(); t += kLook) sent = sent || o.Next(t, false) != Way::None;
        Check(!sent, "a request that took 2.6 s to be taken (slow: Explorer busy) is followed by nothing - neither a press nor the key (they would close Start on its way)");
        Check(o.Done() && !o.Opened() && o.At() >= 2.6 + kWaitAccepted - 1e-9, "...the dock waits kWaitAccepted from when the shell answered");
    }
    {   // clicks before the first request (while the dock reached the shell) are served by that request
        Opening o(true, false);
        o.Again(0);
        o.Again(0);
        Check(o.Next(0, false) == Way::Shell, "clicks while reaching the shell: one request goes out");
        o.Sent(Way::Shell, 0.01, true);
        Check(o.Next(0.04, false) == Way::None && o.Clicks() == 2, "...not a second one at once; the clicks are counted for the log");
    }
    {   // the button closed Start while the wait ran
        Opening o(true, false);
        o.Next(0, false);
        o.Sent(Way::Shell, 0.01, true);
        o.Closed(0.3);
        Check(o.Done() && o.Opened() && o.Next(1.5, false) == Way::None, "closed by the button: the wait ends, nothing more is sent");
    }
    {   // when the key may go out: never brought forward behind something that was taken
        Opening o(true, false);
        Check(o.KeyDue() == 0, "nothing taken yet: nothing holds the key back");
        o.Sent(Way::Shell, 0.5, false);
        Check(o.KeyDue() == 0, "...a request the shell refused holds it back no more");
        Opening a(true, false);
        a.Sent(Way::Shell, 0.5, true);
        Check(a.KeyDue() == 0.5 + kGiveUp, "a request the shell took: the key kGiveUp later");
        a.Sent(Way::Taskbar, 0.5 + kConfirm + 0.2, true);
        Check(a.KeyDue() == 0.5 + kConfirm + 0.2 + kConfirm, "...and no sooner than kConfirm after a press that went through");
        Opening n(true, false);
        n.Sent(Way::Shell, 0.5, true);
        n.Sent(Way::Taskbar, 2.5, false);
        Check(n.KeyDue() == 0.5 + kGiveUp, "a press that did not go through holds the key back no longer than the shell's request does");
    }

    // ---- the usual click: the shell brings Start at once
    {
        Run r = Simulate({});
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell, "usual click: the shell's request brings Start");
        Check(r.shellAsks == 1 && r.keyPresses == 0, "...one request, no Windows key");
        Check(r.taskbarPresses == 0 && r.turnAt < 0, "...and the taskbar's button is not even looked for (its turn did not come)");
        Check(r.o.At() < 0.25, "...and the wait ends with it");
        Check(!r.actedAfterVisible, "nothing is sent once Start is visible");
        Check(r.o.First() == Way::Shell, "...the shell's request is the first way that went out");
    }
    {   // Start already up when the wait begins (the user pressed the Windows key himself meanwhile)
        World w; w.visibleAtClick = true;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.shellAsks == 0 && r.keyPresses == 0 && r.o.CameAfter() == Way::None,
              "Start already visible: nothing is sent (no toggle shut)");
        Check(r.taskbarPresses == 0 && r.o.First() == Way::None, "...no press either: no way went out");
    }

    // ---- the reported case: an administrator's window in front drops the injected key (UIPI)
    {
        World w; w.keyBlocked = true; w.keyReaches = false;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.keyPresses == 0, "...Start comes and no key is pressed into the void");
        w.shellShows = false; w.taskbarShows = false;   // nothing brings it: the shell, the button - and still no key
        Run dud = Simulate(w);
        Check(!dud.o.Opened() && dud.shellAsks == 1 && dud.taskbarPresses == 1 && dud.keyPresses == 0,
              "an administrator's window in front, nothing brings Start: the shell and the button, never the key");
        Check(Near(dud.o.At(), kWaitAccepted), "...the wait ends kWaitAccepted after the shell took the request");
    }

    // ---- right after logon: Start slow to come
    {
        World w; w.shellDelay = 1.8;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell, "cold Start (1.8 s): the shell's request brings it");
        Check(r.keyPresses == 0 && r.cancelled == 0, "...no Windows key that would toggle it shut on its way");
        Check(r.shellAsks == 1, "...and no second request (one can stop the Start on its way)");
        Check(r.taskbarPresses == 0, "...nor a press: the button's turn comes at kConfirm");
    }
    for (double slow : { 2.6, 3.0, 3.5, 5.0, 7.5 }) {   // slower still, the click knowing it is slow: waited on alone
        World w; w.shellDelay = slow; w.slow = 1;
        Run r = Simulate(w);
        char what[160];
        std::snprintf(what, sizeof what, "slow (minutes after start), very cold Start (%.1f s after the request): it comes, nothing else is sent", slow);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell && r.keyPresses == 0 && r.taskbarPresses == 0 && r.cancelled == 0, what);
    }
    for (double slow : { 2.6, 3.0, 3.5 }) {   // not known to be slow: the button's turn comes at kConfirm - its state, on, holds the press back
        World w; w.shellDelay = slow;
        Run r = Simulate(w);
        char what[200];
        std::snprintf(what, sizeof what, "not slow, Start %.1f s after the request, the button's state following it (on): the button is left alone and Start comes", slow);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Shell && r.heldOn && r.taskbarPresses == 0 && r.keyPresses == 0 && r.cancelled == 0, what);
        w.button = false;
        Run none = Simulate(w);
        std::snprintf(what, sizeof what, "...no button at all, Start %.1f s after the request: no key before kGiveUp, Start comes", slow);
        Check(none.o.Opened() && none.o.CameAfter() == Way::Shell && none.keyPresses == 0 && none.cancelled == 0, what);
    }
    {   // the price of the ladder (a design choice, 2026-11-20): a Start slower than the next way's turn, the click not
        // knowing it is slow, is stopped by that way
        World w; w.shellDelay = 5.0;
        Run r = Simulate(w);
        Check(r.cancelled == 1 && r.cancelledBy == Way::Key && Near(r.keyAt, kGiveUp) && r.taskbarPresses == 0,
              "not slow, Start 5 s after the request: the key at kGiveUp toggles it shut (the ladder's price; a slow click waits alone)");
        w.shellDelay = 2.6; w.toggle = Toggle::StaysOff;
        Run p = Simulate(w);
        Check(p.cancelled == 1 && p.cancelledBy == Way::Taskbar && Near(p.pressAt, kConfirm),
              "not slow, Start 2.6 s after the request, the button's state not following it (off): the press at kConfirm stops it");
        Check(p.keyPresses == 1 && Near(p.keyAt, p.pressAt + kConfirm) && p.o.Opened() && p.o.CameAfter() == Way::Key,
              "...and the key, kConfirm after that press, brings Start after all");
    }
    {   // more clicks only ask to be counted
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

    // ---- the shell took the request and Start does not come: the taskbar's button, then the key
    {   // the mirror of 2026-11-20 (then: the button pressed first, off -> on, no Start, 8 s of nothing; the shell's request
        // 2 s into the next click brought it) - now the other way round
        World w; w.shellShows = false;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Taskbar, "the shell takes the request and Start does not come: the taskbar's button brings it");
        Check(Near(r.pressAt, kConfirm) && r.taskbarPresses == 1, "...pressed once, kConfirm after the shell took the request, not before");
        Check(r.shellAsks == 1 && r.keyPresses == 0, "...one request, no key");
        Check(r.o.At() < kConfirm + 0.3, "...in a little over kConfirm, not kWaitAccepted");
        w.clicks = { 0.2, 0.5, 0.8, 1.1, 1.4, 2.6, 3.0 };
        Run more = Simulate(w);
        Check(more.o.Opened() && Near(more.pressAt, kConfirm) && more.taskbarPresses == 1 && more.shellAsks == 1 && more.keyPresses == 0,
              "...more clicks meanwhile change nothing: the same press at the same time, nothing sent for them");
    }
    {   // the button on with Start closed (out of step): left alone - a press would only turn it off
        World w; w.shellShows = false; w.toggle = Toggle::StuckOn;
        Run r = Simulate(w);
        Check(r.heldOn && r.taskbarPresses == 0 && Near(r.turnAt, kConfirm), "the button's state reads on at its turn: not pressed");
        Check(r.keyPresses == 1 && Near(r.keyAt, kGiveUp), "...the key goes out at kGiveUp - not brought forward to the button's turn");
        Check(r.o.Opened() && r.o.CameAfter() == Way::Key, "...and brings Start");
    }
    {   // no button for the dock's monitor (no taskbar there, Windows 10's taskbar, not found): the key waits for its own time
        World w; w.shellShows = false; w.button = false;
        Run r = Simulate(w);
        Check(r.taskbarPresses == 0 && Near(r.turnAt, kConfirm) && r.keyPresses == 1 && Near(r.keyAt, kGiveUp),
              "no button to press: the key at kGiveUp after the shell took the request, not at kConfirm (it toggles: mod.15's rule)");
        Check(r.o.Opened() && r.o.CameAfter() == Way::Key, "...and brings Start");
    }
    {   // the press does not go through: the same
        World w; w.shellShows = false; w.taskbarTakes = false;
        Run r = Simulate(w);
        Check(r.taskbarPresses == 1 && Near(r.pressAt, kConfirm) && r.keyPresses == 1 && Near(r.keyAt, kGiveUp),
              "a press that did not go through, behind a request the shell took: the key still at kGiveUp");
        Opening o(true, false);
        o.Next(0, false);
        o.Sent(Way::Shell, 0.01, false);
        o.Next(0.01, false);
        o.Sent(Way::Taskbar, 0.02, false);
        o.Next(0.04, true);   // Start up meanwhile (the user's own key)
        Check(o.Opened() && o.CameAfter() == Way::None && o.First() == Way::Shell, "...a Start that shows up behind a refused request and such a press is put down to neither");
        Opening alone(false, false);   // (no shell: the press is all that went out)
        alone.Next(0, false);
        alone.Sent(Way::Taskbar, 0.01, false);
        alone.Next(0.04, true);
        Check(alone.Opened() && alone.CameAfter() == Way::None, "...a Start that shows up anyway is not put down to the press that did not go through");
        Opening late(false, false);
        Check(late.Next(6.0, false) == Way::Taskbar, "a late first look still presses the button");
    }
    {   // the press goes through and brings nothing either: the key kConfirm after it
        World w; w.shellShows = false; w.taskbarShows = false;
        Run r = Simulate(w);
        Check(r.taskbarPresses == 1 && r.keyPresses == 1 && Near(r.keyAt, kGiveUp) && r.keyAt - r.pressAt >= kConfirm - 1e-9,
              "the shell and the button both bring nothing: the key at kGiveUp, kConfirm after the press");
        Check(r.o.Opened() && r.o.CameAfter() == Way::Key && r.o.At() < kGiveUp + 0.3, "...brings Start on the first click, about kGiveUp in");
        w.keyReaches = false;
        Run dead = Simulate(w);
        Check(!dead.o.Opened() && dead.shellAsks == 1 && dead.taskbarPresses == 1 && dead.keyPresses == 1 && Near(dead.o.At(), kWaitAccepted),
              "nothing brings Start: each way once, and the dock gives up kWaitAccepted after the shell took the request (and logs what it saw)");
    }
    {   // slow: whatever was taken is waited on alone
        World w; w.shellShows = false; w.slow = 1;
        Run r = Simulate(w);
        Check(!r.o.Opened() && r.taskbarPresses == 0 && r.keyPresses == 0 && r.turnAt < 0,
              "slow (minutes after start), the shell takes the request, Start never comes: nothing else is sent (it could stop a slow Start)");
        Check(Near(r.o.At(), kWaitAccepted), "...the dock waits kWaitAccepted, then logs it");
        Check(r.shellAsks == 1, "...after one request");
        w.slow = 2; w.taskbarShows = false;   // the press call itself was slow (Explorer busy): nothing after it
        Run p = Simulate(w);
        Check(p.taskbarPresses == 1 && Near(p.pressAt, kConfirm) && p.keyPresses == 0 && !p.o.Opened() && Near(p.o.At(), kWaitAccepted),
              "the press itself was slow (Explorer busy): no key after it, the wait ends kWaitAccepted after the shell took the request");
        w.taskbarShows = true; w.taskbarDelay = 5.0;
        Run late = Simulate(w);
        Check(late.o.Opened() && late.o.CameAfter() == Way::Taskbar && late.keyPresses == 0 && late.cancelled == 0,
              "...and a Start that slow press brings 5 s later comes, never toggled shut");
    }
    {   // the press timed out - and reaches Explorer all the same, late
        World w; w.shellShows = false; w.taskbarUnsure = true; w.taskbarDelay = 1.0;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Taskbar && r.keyPresses == 0 && r.cancelled == 0,
              "a press that timed out: nothing else goes out (the shell or the key on top of a late press would toggle twice)");
        w.taskbarShows = false;
        Run dud = Simulate(w);
        Check(!dud.o.Opened() && dud.shellAsks == 1 && dud.keyPresses == 0, "a press that timed out and never arrives: nothing else sent");
        Check(Near(dud.o.At(), kWaitAccepted), "...waited on until kWaitAccepted after the shell took the request");
    }

    // ---- the shell does not help: nothing is on its way, the next way at once
    {
        World w; w.shellAccepts = false; w.button = false;
        Run r = Simulate(w);
        Check(r.o.Opened() && r.o.CameAfter() == Way::Key, "the shell refuses: the Windows key at once brings Start");
        Check(r.keyAt < 0.05, "...without waiting");
        w.button = true;
        Run b = Simulate(w);
        Check(b.o.Opened() && b.o.CameAfter() == Way::Taskbar && b.pressAt <= kLook + 1e-9 && b.keyPresses == 0 && b.shellAsks == 1,
              "the shell refuses, a button to press: the taskbar's button at once brings Start, no key");
    }
    {
        World w; w.shellReady = false; w.button = false;
        Run r = Simulate(w);
        Check(r.o.First() == Way::Key && r.o.Opened() && r.shellAsks == 0, "no launcher (Explorer restarting): the Windows key, as before");
        w.button = true;
        Run b = Simulate(w);
        Check(b.o.First() == Way::Taskbar && b.o.Opened() && b.o.CameAfter() == Way::Taskbar && b.pressAt <= 1e-9 && b.shellAsks == 0 && b.keyPresses == 0,
              "the shell not to be had (its interface gone on a new Windows), a button to press: the taskbar's button at once");
        w.toggle = Toggle::StuckOn;
        Run on = Simulate(w);
        Check(on.heldOn && on.taskbarPresses == 0 && on.keyAt <= 1e-9 && on.o.CameAfter() == Way::Key, "...its state on: left alone, the key at once");
        w.toggle = Toggle::Follows; w.taskbarTakes = false;
        Run k = Simulate(w);
        Check(k.taskbarPresses == 1 && k.keyAt <= kLook + 1e-9, "...its press not through: the key at once");
        Check(k.o.Opened() && k.o.CameAfter() == Way::Key, "...and with no shell either: the Windows key, as before");
        w.taskbarTakes = true; w.taskbarShows = false;
        Run late = Simulate(w);
        Check(late.taskbarPresses == 1 && Near(late.keyAt, late.pressAt + kConfirm) && late.o.CameAfter() == Way::Key,
              "...its press through and bringing nothing: the key kConfirm after the press");
        w.slow = 1;
        Run alone = Simulate(w);
        Check(alone.taskbarPresses == 1 && alone.keyPresses == 0 && !alone.o.Opened() && Near(alone.o.At(), alone.pressAt + kWaitAccepted),
              "...slow: that press is waited on alone for kWaitAccepted, no key");
        w.slow = 0; w.taskbarUnsure = true;
        Run unsure = Simulate(w);
        Check(unsure.taskbarPresses == 1 && unsure.keyPresses == 0 && Near(unsure.o.At(), unsure.pressAt + kWaitAccepted),
              "...its press timed out: waited on alone for kWaitAccepted, nothing else sent");
    }
    {
        World w; w.shellAccepts = false; w.keyBlocked = true; w.keyReaches = false; w.button = false;
        Run r = Simulate(w);
        Check(!r.o.Opened() && r.keyPresses == 0, "shell refuses, key blocked: no key pressed into the void");
        Check(r.shellAsks == 1, "...the shell asked once: a refused request is not repeated");
        Check(r.end < kGiveUp, "...and the dock stops as soon as nothing is left to try");
        Check(r.end <= kLook + 1e-9, "...at once: nothing was taken, nothing is on its way");
    }
    {
        World w; w.shellAccepts = false; w.keyReaches = false; w.button = false;
        Run r = Simulate(w);
        Check(!r.o.Opened() && r.o.Done() && Near(r.o.At(), r.keyAt + kGiveUp),   // (kGiveUp after the key, the last thing taken)
              "nothing brings Start (the shell refuses, the key is dropped): the dock gives up at kGiveUp (and logs what it saw)");
        Check(r.keyPresses == 1 && r.shellAsks == 1, "...after both ways (no request after the key: it might be bringing Start)");
    }
    {
        World w; w.shellReady = false; w.button = false; w.keyInserted = false;
        Run r = Simulate(w);
        Check(!r.o.Opened() && r.end < 0.1, "key not inserted and no shell: the dock stops at once");
    }

    // ---- minutes after Windows or Explorer started: slow
    Check(EarlyAfterStart(kEarlyUptime - 1, -1) && !EarlyAfterStart(kEarlyUptime, -1), "Windows' first kEarlyUptime: slow");
    Check(EarlyAfterStart(864000, 60) && EarlyAfterStart(864000, kEarlyUptime - 0.1) && !EarlyAfterStart(864000, kEarlyUptime),
          "...and Explorer's, Windows up for days (a logon after fast startup, or Explorer restarted)");
    Check(!EarlyAfterStart(864000, -1) && EarlyAfterStart(60, 864000), "...Explorer's age not to be had: Windows' uptime alone; either young is enough");

    // ---- every combination: the rules that must always hold
    portable_check::Quiet() = true;
    int combos = 0;
    const double shellDelays[] = { 0.05, 0.9, 1.9, 2.6, 3.5, 5.0, 7.5 };
    const double taskbarDelays[] = { 0.05, 0.9, 1.9, 2.6, 5.0 };
    const double keyDelays[] = { 0.05, 0.9 };
    const std::vector<std::vector<double>> clickSets = { {}, { 0.15 }, { 0.2, 0.5, 0.8, 1.1, 1.4, 2.6, 3.0 } };
    auto one = [&](const World& w) {
        const Run r = Simulate(w);
        ++combos;
        const std::string d = Describe(w);
        const bool slowFromStart = w.slow == 1;
        const bool shellBrings = w.shellReady && w.shellAccepts && w.shellShows;
        const bool accepted = w.shellReady && w.shellAccepts;
        const bool keyBrings = w.keyInserted && w.keyReaches && !w.keyBlocked;
        const bool pressArrives = w.button && (w.taskbarTakes || w.taskbarUnsure);
        // always ends: kWaitAccepted after the shell took the request; else kWaitAccepted after a press waited on alone,
        // kGiveUp after the last thing taken - and at once when nothing was
        double bound = r.sentAt < 0 ? 0 : r.sentAt;
        if (r.acceptAt >= 0) bound = r.acceptAt + kWaitAccepted;
        else if (r.pressArrives && (w.taskbarUnsure || r.o.IsSlow()) && r.keyPresses == 0) bound = r.pressAt + kWaitAccepted;
        else if (r.takenAt >= 0) bound = r.takenAt + kGiveUp;
        Check(r.o.Done() && r.end <= bound + kLook + 1e-9, ("always ends (kWaitAccepted after the shell took the request; else after what was taken last): " + d).c_str());
        Check(r.shellAsks <= 1 && r.taskbarPresses <= 1 && r.keyPresses <= 1, ("each way at most once per click: " + d).c_str());
        Check(!r.outOfOrder && (w.shellReady ? r.firstWay == Way::Shell : r.shellAsks == 0),
              ("the order: the shell, then the taskbar's button, then the key - the shell first whenever it can be asked: " + d).c_str());
        Check(!r.actedAfterVisible, ("nothing sent once Start is visible: " + d).c_str());
        Check(!r.tooSoonAfterAccept, ("nothing within kConfirm of a request the shell took: " + d).c_str());
        Check(!r.sentAfterTakenSlow, ("slow: nothing after something was taken (waited on alone): " + d).c_str());
        Check(!r.keyTooSoon, ("the key never sooner than kGiveUp after a request the shell took, nor than kConfirm after a press that went through: " + d).c_str());
        Check(!r.sentAfterUnsure, ("nothing after a press that may still arrive: " + d).c_str());
        Check(!(w.keyBlocked && r.keyPresses), ("no key into an administrator's window: " + d).c_str());
        Check(!(r.heldOn && r.taskbarPresses), ("a button whose state reads on is not pressed: " + d).c_str());
        if (r.o.Opened()) Check(r.o.CameAfter() != Way::None, ("Start's coming is put down to a way: " + d).c_str());
        if (!shellBrings && !(pressArrives && w.taskbarShows) && !keyBrings) Check(!r.o.Opened(), ("nothing can bring Start: not reported as opened: " + d).c_str());
        if (slowFromStart && accepted)
            Check(r.taskbarPresses == 0 && r.keyPresses == 0 && r.turnAt < 0, ("slow from the start, the shell took the request: it is waited on alone, the button not even looked for: " + d).c_str());
        // a Start on its way is stopped by the dock only when it was slower than the next way's turn and the click not slow
        if (r.cancelled) {
            Check(!r.cancelledSlow && r.cancelledDelay >= kConfirm - 1e-9, ("a Start on its way is only ever stopped when slower than kConfirm, the click not slow: " + d).c_str());
            Check(r.cancelledBy != Way::Taskbar || w.toggle == Toggle::StaysOff, ("a button whose state follows Start is never pressed onto a Start on its way: " + d).c_str());
            if (r.cancelledBy == Way::Key && r.cancelledOf == Way::Shell)
                Check(r.cancelledDelay >= kGiveUp - 1e-9, ("the key stops the shell's Start only when that is slower than kGiveUp: " + d).c_str());
            Check(r.cancelled == 1, ("...and at most once a click: " + d).c_str());
        }
        // the shell's Start in time comes by the shell alone
        if (shellBrings && w.shellDelay < (slowFromStart ? kWaitAccepted : kConfirm) - 0.1)
            Check(r.o.Opened() && r.o.CameAfter() == Way::Shell && r.cancelled == 0 && r.taskbarPresses == 0 && r.keyPresses == 0,
                  ("a Start the shell brings within kConfirm (slow: within kWaitAccepted) comes by it alone: " + d).c_str());
        if (shellBrings && w.shellDelay < kGiveUp - 0.1 && (!w.button || w.toggle != Toggle::StaysOff))
            Check(r.o.Opened() && r.o.CameAfter() == Way::Shell && r.cancelled == 0 && r.taskbarPresses == 0 && r.keyPresses == 0,
                  ("no button, or one whose state reads on: a Start the shell brings within kGiveUp still comes by it alone: " + d).c_str());
        // the shell took the request and brings nothing (2026-11-20 the other way round): the button at kConfirm
        if (accepted && !w.shellShows && w.slow != 1 && w.button && w.toggle != Toggle::StuckOn) {
            Check(r.taskbarPresses == 1 && Near(r.pressAt, r.acceptAt + kConfirm), ("the shell took the request and nothing came: the button pressed at kConfirm: " + d).c_str());
            if (pressArrives && w.taskbarShows && w.taskbarDelay < kConfirm - 0.1)
                Check(r.o.Opened() && r.o.CameAfter() == Way::Taskbar && r.keyPresses == 0 && r.cancelled == 0, ("...and the Start it brings in time comes by it, no key: " + d).c_str());
        }
        // ...and the key at kGiveUp when no press went through, the click not slow
        if (accepted && !w.shellShows && w.slow == 0 && !w.keyBlocked && !(w.button && w.toggle != Toggle::StuckOn && (w.taskbarTakes || w.taskbarUnsure))) {
            Check(r.keyPresses == 1 && Near(r.keyAt, r.acceptAt + kGiveUp), ("the shell took the request, no press went through: the key at kGiveUp, not sooner: " + d).c_str());
            if (keyBrings) Check(r.o.Opened() && r.o.CameAfter() == Way::Key, ("...and brings Start: " + d).c_str());
        }
        // the shell did not take a request: nothing is on its way - the next way at once
        if (!accepted) {
            if (w.button && w.toggle != Toggle::StuckOn) Check(r.taskbarPresses == 1 && r.pressAt <= kLook + 1e-9, ("no request taken: the button at once: " + d).c_str());
            else if (!w.keyBlocked) Check(r.keyPresses == 1 && r.keyAt <= kLook + 1e-9, ("no request taken, no button to press: the key at once: " + d).c_str());
            else Check(r.end <= kLook + 1e-9 && !r.o.Opened(), ("no request taken, no button, the key blocked: the dock stops at once: " + d).c_str());
        }
        // a button never pressed (its turn did not come, or its state read on) or pressed in vain (not through, the press
        // call not slow): the click goes on as without a button - the same ways, the key within a look of when it would
        if (w.button && (r.taskbarPresses == 0 || (!w.taskbarTakes && !w.taskbarUnsure && w.slow != 2))) {
            World plain = w;
            plain.button = false;
            if (plain.slow == 2) plain.slow = 0;   // (no press: nothing was slow)
            const Run q = Simulate(plain);
            const bool keySame = r.keyPresses == q.keyPresses && (!r.keyPresses || (r.keyAt >= q.keyAt - 1e-9 && r.keyAt <= q.keyAt + kLook + 1e-9));
            Check(r.o.Opened() == q.o.Opened() && r.o.CameAfter() == q.o.CameAfter() && r.shellAsks == q.shellAsks && keySame,
                  ("a button left alone, or a press that did not go through: the click goes on as without a button: " + d).c_str());
        }
        // more clicks change nothing that is sent
        if (!w.clicks.empty()) {
            World quiet = w;
            quiet.clicks.clear();
            const Run q = Simulate(quiet);
            int taken = 0;   // the clicks that came before the wait was over
            for (double c : w.clicks) taken += c <= r.end;
            Check(r.shellAt == q.shellAt && r.pressAt == q.pressAt && r.keyAt == q.keyAt && r.o.Opened() == q.o.Opened() && r.o.CameAfter() == q.o.CameAfter() &&
                  r.o.At() == q.o.At() && r.o.Clicks() == taken,
                  ("more clicks while waiting: counted, and what is sent, and when, is the same as without them: " + d).c_str());
        }
    };
    for (int shell = 0; shell < 8; ++shell)
        for (double sdelay : shellDelays)
            for (int key = 0; key < 8; ++key)
                for (double kdelay : keyDelays)
                    for (const auto& clicks : clickSets) {
                        World w;
                        w.shellReady = shell & 1; w.shellAccepts = shell & 2; w.shellShows = shell & 4;
                        w.keyInserted = key & 1; w.keyReaches = key & 2; w.keyBlocked = key & 4;
                        w.shellDelay = sdelay; w.keyDelay = kdelay; w.clicks = clicks;
                        // no button for the dock's monitor (slow or not; the press's axes say nothing then)
                        w.button = false;
                        for (int slow = 0; slow < 2; ++slow) { w.slow = slow; one(w); }
                        // a button: its state following Start, staying off, or stuck on; its press going through, not, or
                        // perhaps (a timeout); bringing Start fast, slowly or never; the click slow from the start, slow by the
                        // press, or not
                        w.button = true;
                        for (int toggle = 0; toggle < 3; ++toggle)
                            for (int press = 0; press < 3; ++press)
                                for (int shows = 0; shows < 2; ++shows)
                                    for (double tdelay : taskbarDelays)
                                        for (int slow = 0; slow < 3; ++slow) {
                                            w.toggle = static_cast<Toggle>(toggle);
                                            w.taskbarTakes = press == 0; w.taskbarUnsure = press == 2;
                                            w.taskbarShows = shows; w.taskbarDelay = tdelay; w.slow = slow;
                                            one(w);
                                        }
                    }
    portable_check::Quiet() = false;
    std::printf("checked %d combinations\n", combos);
    return Summary("start_menu_test");
}
