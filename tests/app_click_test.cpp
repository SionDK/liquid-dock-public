// What a click on an app's dock icon does (src/app_click.h): minimize only the active window that is
// not minimized (Explorer's rule), bring everything else up; pass over windows hidden since the app's
// list was made; prefer a window on a screen; clicks while a window is coming bring that one up again;
// what the dock does while it waits; Start or Search in front at the click, closed first. No Windows headers: builds with any C++20 compiler.
//   cl /std:c++20 /EHsc /utf-8 tests\app_click_test.cpp
//   g++ -std=c++20 tests/app_click_test.cpp -o app_click_test
#include "../src/app_click.h"
#include "portable_check.h"
#include <string>

using namespace appclick;

namespace {
Win W(uint64_t id, bool minimized = false, bool shown = true, bool onScreen = true) {
    Win w;
    w.id = id; w.minimized = minimized; w.shown = shown; w.onScreen = onScreen && !minimized;
    return w;
}
bool Is(const Decision& d, Action a, uint64_t window, Why why) { return d.action == a && d.window == window && d.why == why; }
}

int main() {
    // ---- one window
    Check(Is(Decide({}, 0), Action::Launch, 0, Why::NoWindow), "no window: start the app");
    Check(Is(Decide({ W(1) }, 1), Action::Minimize, 1, Why::Active), "its window is the active one: minimize it");
    Check(Is(Decide({ W(1) }, 9), Action::Activate, 1, Why::Front), "another program is active: bring it up");
    Check(Is(Decide({ W(1) }, 0), Action::Activate, 1, Why::Front), "nothing is active: bring it up");
    Check(Is(Decide({ W(1, true) }, 9), Action::Activate, 1, Why::Minimized), "minimized, another program active: restore it");

    // the reported case: a window minimized (or closed to a corner of the screen) on the other monitor
    // stays the active window when nothing else takes the activation. The old rule minimized it again
    // ("the first click does nothing, the next one opens it").
    Decision d = Decide({ W(1, true) }, 1);
    Check(d.action == Action::Activate && d.window == 1, "its minimized window is still the active one: bring it back, never minimize again");
    Check(d.why == Why::ActiveMinimized, "(and the log says why)");

    // ---- several windows: cycle while one of them is active
    Check(Is(Decide({ W(1), W(2), W(3) }, 1), Action::Activate, 3, Why::Cycle), "one of its windows active: the bottom-most other comes up");
    Check(Is(Decide({ W(1), W(2), W(3) }, 3), Action::Activate, 2, Why::Cycle), "the bottom-most is the active one: the next above it");
    Check(Is(Decide({ W(1), W(2, true) }, 1), Action::Activate, 2, Why::Cycle), "cycling restores a minimized one");
    Check(Is(Decide({ W(1, true), W(2) }, 1), Action::Activate, 1, Why::ActiveMinimized), "the active one minimized: it comes back (no cycling)");
    Check(Is(Decide({ W(1), W(2) }, 9), Action::Activate, 1, Why::Front), "another program active: its topmost window");
    Check(Is(Decide({ W(1, true), W(2) }, 9), Action::Activate, 2, Why::Front), "a window on the screen before a minimized one");
    Check(Is(Decide({ W(1, true), W(2, true) }, 9), Action::Activate, 1, Why::Minimized), "all minimized: the topmost is restored");

    // ---- windows that no longer show (the list is up to a second old)
    d = Decide({ W(1, false, false), W(2) }, 1);
    Check(Is(d, Action::Activate, 2, Why::Front) && d.skipped == 1,
          "a window hidden meanwhile is passed over, even while it is still the active one (programs that hide without activating another)");
    d = Decide({ W(1, false, false) }, 1);
    Check(Is(d, Action::Launch, 0, Why::NoWindow) && d.skipped == 1, "its only window hidden meanwhile (to the tray): start it, so an app that runs once shows its window");
    d = Decide({ W(1, false, false), W(2, false, false) }, 0);
    Check(d.action == Action::Launch && d.skipped == 2, "all hidden: start it");

    // ---- click-through overlays (a game overlay of a chat app, say) are never the window brought up
    Win overlay = W(7);
    overlay.clickThrough = true;
    d = Decide({ overlay, W(2, true) }, 9);
    Check(Is(d, Action::Activate, 2, Why::Minimized) && d.overlays == 1, "an overlay on top: its minimized main window is restored instead");
    d = Decide({ overlay, W(1, false, false) }, 1);
    Check(Is(d, Action::Launch, 0, Why::NoWindow) && d.overlays == 1 && d.skipped == 1,
          "main window hidden to the tray, only the overlay left: start it (an app that runs once shows its window)");
    d = Decide({ overlay }, 7);
    Check(d.action == Action::Launch, "even when the overlay is the active window, it is not minimized or brought up");

    // ---- off the screens
    Check(Is(Decide({ W(1, false, true, false), W(2) }, 9), Action::Activate, 2, Why::Front), "a window off the screens comes after one on a screen");
    Check(Is(Decide({ W(1, false, true, false), W(2, true) }, 9), Action::Activate, 2, Why::Minimized), "and after a minimized one (restoring shows it)");
    Check(Is(Decide({ W(1, false, true, false), W(2, false, true, false) }, 9), Action::Activate, 1, Why::OffScreen), "only off the screens: the topmost anyway");
    Check(Is(Decide({ W(1, false, true, false) }, 1), Action::Minimize, 1, Why::Active), "an active window off the screens is still minimized like any other");

    // ---- clicked again while its window is coming (a program slow to answer, 2026-10-26).
    // The old rule restarted the wait each click and minimized the window on the click after it came.
    Check(Is(Decide({ W(1, true) }, 9, 1), Action::Activate, 1, Why::Again), "still coming, still minimized: the same window again");
    Check(Is(Decide({ W(1) }, 1, 1), Action::Activate, 1, Why::Again), "it came while the user clicked again: not minimized by that click");
    Check(Is(Decide({ W(1, true) }, 1, 1), Action::Activate, 1, Why::Again), "active but still minimized while coming: again");
    Check(Is(Decide({ W(1), W(2, true) }, 1, 2), Action::Activate, 2, Why::Again), "several windows: the one coming, not the next in the cycle");
    Check(Is(Decide({ W(1) }, 1, 0, 1), Action::Activate, 1, Why::Again), "came up a moment ago: the next click does not minimize it");
    Check(Is(Decide({ W(1) }, 1, 0, 2), Action::Minimize, 1, Why::Active), "another window came up a moment ago: this one is minimized as usual");
    Check(Is(Decide({ W(1), W(2) }, 1, 0, 1), Action::Activate, 2, Why::Cycle), "several windows, one just came: the click cycles on (nothing minimized)");
    Check(Is(Decide({ W(1) }, 1, 7), Action::Minimize, 1, Why::Active), "a window coming that is not this app's: the usual rule");
    d = Decide({ W(1, false, false), W(2) }, 9, 1);
    Check(Is(d, Action::Activate, 2, Why::Front) && d.skipped == 1, "the window coming was hidden meanwhile: the usual choice");
    Check(Decide({ overlay }, 7, 7).action == Action::Launch, "an overlay is never the window coming");
    Check(Is(Decide({ W(1, false, false), W(2) }, 2, 1), Action::Minimize, 2, Why::Active),
          "the window coming hidden meanwhile, the other one active: minimized as usual (the click is a new wish)");
    Check(Is(Decide({ W(1), W(2, true) }, 1, 2, 1), Action::Activate, 2, Why::Again), "one coming and another just up: the one coming");
    Check(Is(Decide({ overlay, W(2) }, 7), Action::Activate, 2, Why::Front), "its overlay in front: not its window active - its window comes up, nothing minimized");
    Check(Is(Decide({ W(1), W(2, false, false), W(3) }, 1), Action::Activate, 3, Why::Cycle), "cycling passes over a window hidden meanwhile");
    Check(Is(Decide({ W(1), W(2), W(3, false, false) }, 2), Action::Activate, 1, Why::Cycle), "...the bottom-most shown one active: the one above it");
    Check(Is(Decide({ W(1, true), W(2, true) }, 2), Action::Activate, 2, Why::ActiveMinimized), "the active one of several minimized: it comes back (no cycling to another)");
    Check(Is(Decide({ W(1), W(2), W(3, false, true, false) }, 1), Action::Activate, 2, Why::Cycle),
          "cycling passes over a window off the screens while another one is on a screen (bringing that one up shows nothing)");
    Check(Is(Decide({ W(1), W(2, true), W(3, false, true, false) }, 1), Action::Activate, 2, Why::Cycle),
          "...a minimized one before it too (restoring shows it)");
    Check(Is(Decide({ W(1), W(2, false, true, false) }, 1), Action::Activate, 2, Why::Cycle),
          "...but the only other one off the screens: still cycled to (one of several is never minimized)");
    d = Decide({ W(1), W(2), W(3, false, true, false), W(4, false, true, false) }, 1);
    Check(Is(d, Action::Activate, 2, Why::Cycle) && d.offScreen == 2, "...two off the screens passed over: counted for the log");
    d = Decide({ W(1), W(2, false, true, false), W(3, false, true, false) }, 1);
    Check(Is(d, Action::Activate, 3, Why::Cycle) && d.offScreen == 0, "...all the others off the screens: the bottom-most, nothing passed over");
    d = Decide({ W(1), W(2), W(3, false, true, false) }, 3);
    Check(Is(d, Action::Activate, 2, Why::Cycle) && d.offScreen == 0, "...the one off the screens is the active one: the bottom-most other, as before");
    Check(Decide({ W(1), W(2), W(3) }, 1).offScreen == 0 && Decide({ W(1, false, true, false), W(2) }, 9).offScreen == 0,
          "nothing off the screens passed over in a cycle: none counted (Front counts none)");

    // ---- waiting for it to come (Next)
    Look l;
    l.cameUp = true;
    Check(Next(l) == Step::Done, "came: done");
    l = Look{}; l.cameUp = true; l.elsewhere = true;
    Check(Next(l) == Step::Done, "came, and the user went on: it came");
    l = Look{}; l.gone = true;
    Check(Next(l) == Step::Stop, "window gone: stop");
    l = Look{}; l.elsewhere = true; l.minimized = true;
    Check(Next(l) == Step::Stop, "the user went on to another program before it came: stop");
    l = Look{}; l.restoring = true; l.minimized = true; l.since = 3;
    Check(Next(l) == Step::Wait, "its program has not answered the restore yet: wait, send nothing more");
    l.since = kBusyWait + 0.1;
    Check(Next(l) == Step::GiveUp, "no answer in kBusyWait: give up (a line in the log)");
    l = Look{}; l.minimized = true; l.sinceHandled = 0.2;
    Check(Next(l) == Step::Wait, "restore just handled: time for the window to show");
    l.sinceHandled = kAfterRestore + 0.1;
    Check(Next(l) == Step::GiveUp, "handled a while ago and still down: give up");
    l = Look{}; l.minimized = true;
    Check(Next(l) == Step::Restore, "minimized and no restore sent (it went down after the click): send one");
    l = Look{}; l.shown = false;
    Check(Next(l) == Step::GiveUp, "hidden by its program, nothing to restore: give up");
    l = Look{}; l.failed = true; l.minimized = true;
    Check(Next(l) == Step::GiveUp, "the command was not delivered: give up");
    l = Look{};
    Check(Next(l) == Step::Refocus, "up but another window in front: put it in front again");
    l.refocused = kRefocusTries;
    Check(Next(l) == Step::GiveUp, "still behind after the tries: give up");
    l = Look{}; l.gone = true; l.cameUp = true;
    Check(Next(l) == Step::Stop, "gone, whatever else: stop");
    l = Look{}; l.restoring = true; l.cameUp = true;
    Check(Next(l) == Step::Done, "it came while its restore is unanswered (it came another way): done, nothing waited for");
    l = Look{}; l.restoring = true; l.minimized = true; l.elsewhere = true;
    Check(Next(l) == Step::Stop, "the user went on while its program is busy: stop (no line saying it did not come)");
    l = Look{}; l.restoring = true; l.minimized = true; l.since = kBusyWait;
    Check(Next(l) == Step::GiveUp, "...at kBusyWait exactly: give up");
    l = Look{}; l.failed = true;
    Check(Next(l) == Step::GiveUp, "up but behind, the restore not delivered: give up (the log says why)");
    l = Look{}; l.shown = false; l.sinceHandled = 0.1;
    Check(Next(l) == Step::Wait, "hidden right after its program handled the restore: time to show");
    l.sinceHandled = kAfterRestore;
    Check(Next(l) == Step::GiveUp, "...still hidden after kAfterRestore: give up");
    l = Look{}; l.refocused = kRefocusTries - 1;
    Check(Next(l) == Step::Refocus, "up but behind, one try left: put it in front again");
    Check(kLook > 0.1 && kLook < 0.5 && kBusyNote >= kLook && kBusyWait >= 3 && kAfterRestore >= kLook && kJustUp < 1.0,
          "looks come soon after the click; a busy program gets seconds");

    // ---- Start or Search in front at the click: closed with Esc first (NextForPopup)
    Check(NextForPopup(false, 0, 0) == PopupStep::Gone, "pop-up not in front (gone before any Esc): bring the window up now");
    Check(NextForPopup(false, 1, 0) == PopupStep::Gone, "gone right after the Esc: bring it up now (no wait for kPopupGone)");
    Check(NextForPopup(false, kPopupTries, 99) == PopupStep::Gone, "gone after the last Esc, however late: bring it up (no give-up)");
    Check(NextForPopup(true, 0, 0) == PopupStep::Close, "in front, no Esc sent yet: Esc");
    Check(NextForPopup(true, 0, 99) == PopupStep::Close, "...whatever time it says (none was sent)");
    Check(NextForPopup(true, 1, 0) == PopupStep::Wait, "Esc just sent, still in front: closing - wait, no second Esc");
    Check(NextForPopup(true, 1, kPopupGone - 0.01) == PopupStep::Wait, "...just before kPopupGone: still waiting");
    Check(NextForPopup(true, 1, kPopupGone) == PopupStep::Close, "...kPopupGone after the Esc exactly, still in front: Esc again");
    Check(NextForPopup(true, kPopupTries, kPopupGone - 0.01) == PopupStep::Wait, "the last Esc a moment ago: time to close, no give-up yet");
    Check(NextForPopup(true, kPopupTries, kPopupGone) == PopupStep::GiveUp, "still in front kPopupGone after the last Esc: give up (a line in the log)");
    Check(NextForPopup(true, kPopupTries + 1, 10) == PopupStep::GiveUp, "more Esc than the tries (never sent so): give up, no more keys");
    {   // the whole wait for a pop-up that never closes, looked at every kPopupLook: kPopupTries Esc, then the give-up
        int esc = 0, lookAt = 0, closedAt = 0;
        PopupStep s = PopupStep::Wait;
        for (int i = 0; i < 100; ++i) {
            s = NextForPopup(true, esc, (i - closedAt) * kPopupLook);
            if (s == PopupStep::Close) { ++esc; closedAt = i; }
            if (s == PopupStep::GiveUp || s == PopupStep::Gone) { lookAt = i; break; }
        }
        const double gaveUp = lookAt * kPopupLook;
        Check(s == PopupStep::GiveUp && esc == kPopupTries, "a pop-up that stays: Esc kPopupTries times, then give up");
        Check(gaveUp >= kPopupTries * kPopupGone - 1e-9 && gaveUp <= kPopupTries * kPopupGone + 2 * kPopupLook,
              "...the give-up kPopupGone after the last Esc (about a second after the click)");
    }
    Check(kPopupLook > 0 && kPopupLook < kPopupGone && kPopupLook < kLook && kPopupTries >= 2 && kPopupTries * kPopupGone <= 1.5,
          "pop-up looks come sooner than the window's; Esc at least twice; the give-up within a second and a half");

    // ---- the result check
    Check(CameUp(true, false, true, false), "shown, not minimized, in front: came up");
    Check(CameUp(true, false, false, true), "another window of the same program in front (a dialog it opened): came up");
    Check(!CameUp(true, true, true, true), "still minimized: not yet");
    Check(!CameUp(false, false, true, true), "hidden: no");
    Check(!CameUp(true, false, false, false), "shown but another program in front: no");
    Check(!CameUp(true, false, false, true, true),
          "the same program's other window, in front since before the click (a browser profile, a store app's frame): no sign it came");
    Check(CameUp(true, false, true, false, true), "it is the window in front: came, whatever was in front at the click");
    Check(!CameUp(true, true, true, true, false) && !CameUp(false, false, false, true, false), "still minimized or hidden: not yet, whoever is in front");

    bool named = true;
    for (Why w : { Why::NoWindow, Why::Active, Why::Cycle, Why::ActiveMinimized, Why::Front, Why::Minimized, Why::OffScreen, Why::Again })
        named = named && std::wstring(WhyText(w)).size() > 5;
    Check(named, "every decision has words for the log");
    return Summary("app_click_test");
}
