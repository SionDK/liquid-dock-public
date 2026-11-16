// The FluentFlyout module's decisions (src/fluent_logic.h), with no Windows headers: it builds with any
// C++20 compiler.
//   cl /std:c++20 /EHsc /utf-8 tests\fluent_logic_test.cpp        (Windows, run_regressions.bat)
//   g++ -std=c++20 tests/fluent_logic_test.cpp -o fluent_logic_test (Linux, WSL, a local LLM box)
//
// Besides the single functions, a small model of FluentFlyout's own choice of "the taskbar" (v2.15,
// TaskbarWindow.GetSelectedTaskbarHandle) runs against the dock's rule for every monitor layout up to
// three monitors, every setting and every state of the settings file: the dock's host window has to end
// up where FluentFlyout puts the widget, stay there (no back and forth), and follow a changed setting.
#include "../src/fluent_logic.h"
#include "portable_check.h"
#include <string>

using namespace fluentlogic;

namespace {

// ---------------------------------------------------------------- a model of both sides
// FluentFlyout, every 1.5 s: the first Shell_TrayWnd (the dock's host window, kept first in z-order)
// when it is on the selected monitor; with one monitor that one anyway; otherwise the secondary
// Explorer taskbar on the selected monitor (Explorer has one on every monitor but the primary); and
// when there is none (the selected monitor is the primary), the first Shell_TrayWnd again.
struct Model {
    int monitors = 2, primary = 0;
    int selected = 0;          // FluentFlyout's setting, as a monitor position: what it really uses
    int host = -1;             // the monitor of the dock's host window
    int inExplorer = -1;       // the widget is in Explorer's secondary taskbar on this monitor (-1: not)
    bool inMain = false;       // ...in Explorer's main taskbar (before the dock's host window is up)
    bool secondaries = true;   // Explorer has a taskbar on every other monitor ("show my taskbar on all displays")

    void FluentTick() {
        inMain = false;
        if (host == selected || monitors == 1) { inExplorer = -1; return; }
        inExplorer = secondaries && selected != primary ? selected : -1;
    }
    // The dock starting: its host window is not up, the first Shell_TrayWnd is Explorer's main taskbar.
    void FluentTickBeforeHost() {
        inExplorer = -1;
        inMain = !(secondaries && selected != primary && monitors > 1);
        if (!inMain) inExplorer = selected;
    }
};

// The dock: every pass it notes where FluentFlyout put the widget, where that tells its monitor
// (fluent_flyout.cpp WatchWidgetParent), and moves its host window to the monitor
// fluentlogic::WantedMonitor names.
struct Dock {
    Choice c;
    void Tick(const Model& m, double now) {
        int in = m.inExplorer >= 0 ? m.inExplorer : m.inMain ? m.primary : -1;
        if (in >= 0 && ShowsMonitor(m.inExplorer >= 0, m.secondaries ? m.monitors - 1 : 0, m.monitors)) { c.shownMonitor = in; c.shownAt = now; }
    }
    int Wanted(int primary) const { return WantedMonitor(c, primary); }
};

struct Outcome {
    int host = -1;
    bool inHost = false;
    int moves = 0;             // how often the host window changed monitor
    int lateMoves = 0;         // ...after the first few FluentFlyout ticks (back and forth)
};

// Runs both sides: the dock several times per FluentFlyout tick. The host window starts on the primary
// monitor (before hosting it mirrors Explorer's taskbar there).
Outcome Run(Model& m, Dock& d, double& now, int fluentTicks) {
    Outcome o;
    if (m.host < 0) m.host = m.primary;
    for (int t = 0; t < fluentTicks; ++t) {
        for (int pass = 0; pass < 4; ++pass) {
            d.Tick(m, now);
            int want = d.Wanted(m.primary);
            if (want != m.host) { ++o.moves; if (t >= 3) ++o.lateMoves; m.host = want; }
            now += 0.25;
        }
        m.FluentTick();
        now += 0.5;
    }
    d.Tick(m, now);
    o.host = m.host;
    o.inHost = m.inExplorer < 0 && !m.inMain;
    return o;
}

// The dock starting: it finds the widget where FluentFlyout put it before the host window was up, then
// puts its host window up on the monitor it wants; FluentFlyout goes on as usual.
Outcome RunFromStart(Model& m, Dock& d, double& now, int fluentTicks) {
    m.FluentTickBeforeHost();
    d.Tick(m, now);
    m.host = d.Wanted(m.primary);
    return Run(m, d, now, fluentTicks);
}

std::string Label(const char* what, int n, int primary, int selected, int file) {
    return std::string(what) + " (monitors " + std::to_string(n) + ", primary " + std::to_string(primary) +
           ", FluentFlyout set to " + std::to_string(selected) + ", file " + std::to_string(file) + ")";
}

}   // namespace

int main() {
    // ---------------------------------------------------------------- FluentFlyout's numbering
    {
        std::vector<Monitor> one = { { true, 0 } };
        Check(FluentOrder(one) == std::vector<int>{ 0 }, "one monitor: number 1");
        Check(MonitorForSetting(one, 0) == 0 && MonitorForSetting(one, 1) == 0 && MonitorForSetting(one, -2) == 0,
              "one monitor: every setting means it (FluentFlyout clamps)");

        // a two-monitor layout: main 2560x1440 at 0,0, a portrait monitor on its left at -1440
        std::vector<Monitor> mine = { { true, 0 }, { false, -1440 } };
        Check(FluentOrder(mine) == std::vector<int>({ 0, 1 }), "main first although the other one is further left");
        Check(MonitorForSetting(mine, 0) == 0 && MonitorForSetting(mine, 1) == 1, "setting 0 = main, setting 1 = the portrait one");
        Check(SettingNumber(mine, 1) == 2 && SettingNumber(mine, 0) == 1, "FluentFlyout shows them as \"1\" (main) and \"2\"");

        std::vector<Monitor> listedFirst = { { false, -1440 }, { true, 0 } };   // the system lists the other one first
        Check(MonitorForSetting(listedFirst, 0) == 1 && MonitorForSetting(listedFirst, 1) == 0, "the system's order does not matter");

        std::vector<Monitor> three = { { false, 2560 }, { true, 0 }, { false, -1920 } };
        Check(FluentOrder(three) == std::vector<int>({ 1, 2, 0 }), "three monitors: main, then left to right");
        Check(MonitorForSetting(three, 9) == 0 && MonitorForSetting(three, -1) == 1, "out-of-range settings are clamped");

        std::vector<Monitor> stacked = { { false, 0 }, { true, 0 }, { false, 0 } };   // above each other: same left
        Check(FluentOrder(stacked) == std::vector<int>({ 1, 0, 2 }), "equal left edges keep the system's order");

        std::vector<Monitor> none;
        Check(MonitorForSetting(none, 0) == -1 && PrimaryOf(none) == -1 && SettingNumber(none, 0) == 0, "no monitors: nothing");
        std::vector<Monitor> noPrimary = { { false, 10 }, { false, -10 } };
        Check(PrimaryOf(noPrimary) == 0, "no monitor marked primary: the first one");

        bool roundTrip = true;
        for (auto* layout : { &one, &mine, &listedFirst, &three, &stacked })
            for (int k = 0; k < (int)layout->size(); ++k)
                roundTrip &= SettingNumber(*layout, MonitorForSetting(*layout, k)) == k + 1;
        Check(roundTrip, "SettingNumber is the inverse of MonitorForSetting");
    }

    // ---------------------------------------------------------------- WantedMonitor
    {
        Choice c;
        Reason why = Reason::File;
        Check(WantedMonitor(c, 3, &why) == 3 && why == Reason::Default, "nothing known: the primary monitor");
        c.fileMonitor = 1; c.fileAt = 10;
        Check(WantedMonitor(c, 0, &why) == 1 && why == Reason::File, "the settings file");
        c = {}; c.shownMonitor = 2; c.shownAt = 5;
        Check(WantedMonitor(c, 0, &why) == 2 && why == Reason::Shown, "where FluentFlyout put it");
        c.fileMonitor = 1; c.fileAt = 4;
        Check(WantedMonitor(c, 0) == 2, "FluentFlyout showed it after the file was read: what it did counts");
        c.fileAt = 6;
        Check(WantedMonitor(c, 0) == 1, "the setting changed after that: the file counts");
        c.fileAt = 5;
        Check(WantedMonitor(c, 0) == 2, "same moment: what FluentFlyout did counts");
    }

    // ---------------------------------------------------------------- both sides together
    {
        int cases = 0, converged = 0, stable = 0, trapped = 0;
        bool allConverged = true, allStable = true, trapAsDocumented = true, followsChange = true;
        portable_check::Quiet() = true;
        for (int n = 1; n <= 3; ++n)
            for (int primary = 0; primary < n; ++primary)
                for (int selected = 0; selected < n; ++selected)
                    for (int file = -1; file < n; ++file) {
                        ++cases;
                        Model m; m.monitors = n; m.primary = primary; m.selected = selected;
                        Dock d; d.c.fileMonitor = file; d.c.fileAt = file >= 0 ? 0.0 : -1.0;
                        double now = 0;
                        Outcome o = Run(m, d, now, 12);
                        const bool fileRight = file < 0 || file == selected;
                        // FluentFlyout set to the main monitor puts its widget into the host window wherever
                        // that is: then only the file can tell, and a wrong file wins (a stale file only)
                        const bool trap = !fileRight && selected == primary && n > 1;
                        if (trap) {
                            ++trapped;
                            trapAsDocumented &= Check(o.inHost && o.host == file, Label("FluentFlyout set to main, the file says otherwise: the file's monitor", n, primary, selected, file).c_str());
                        } else {
                            bool ok = o.inHost && o.host == selected;
                            converged += ok;
                            allConverged &= Check(ok, Label("ends on FluentFlyout's monitor, in the host window", n, primary, selected, file).c_str());
                        }
                        bool calm = o.lateMoves == 0 && o.moves <= 2;
                        stable += calm;
                        allStable &= Check(calm, Label("no back and forth", n, primary, selected, file).c_str());

                        // then the setting is changed in FluentFlyout (and saved): the dock follows
                        for (int next = 0; next < n; ++next) {
                            if (next == selected) continue;
                            Model m2 = m; Dock d2 = d;
                            m2.selected = next;
                            now += 1;
                            d2.c.fileMonitor = next; d2.c.fileAt = now;
                            Outcome o2 = Run(m2, d2, now, 6);
                            followsChange &= Check(o2.inHost && o2.host == next && o2.lateMoves == 0,
                                                   Label("follows a changed setting", n, primary, next, next).c_str());
                        }
                    }
        portable_check::Quiet() = false;
        Check(allConverged, ("every layout and setting with a right or unknown file ends right (" + std::to_string(converged) + " cases)").c_str());
        Check(trapAsDocumented, ("main monitor chosen in FluentFlyout but a stale file: the file decides (" + std::to_string(trapped) + " cases)").c_str());
        Check(allStable, ("the host window never goes back and forth (" + std::to_string(stable) + " of " + std::to_string(cases) + ")").c_str());
        Check(followsChange, "a setting changed in FluentFlyout is followed, from every start");

        // unsaved change to another monitor: what FluentFlyout does shows it
        Model m; m.monitors = 2; m.primary = 0; m.selected = 1;
        Dock d; d.c.fileMonitor = 1; d.c.fileAt = 0;
        double now = 0;
        Run(m, d, now, 6);
        m.selected = 0;                      // changed back to main in FluentFlyout, file not saved: the documented gap
        Outcome stuck = Run(m, d, now, 6);
        Check(stuck.host == 1 && stuck.inHost, "set back to main without saving: stays (only the file can tell)");
        m.selected = 1; Run(m, d, now, 3);
        Model m3; m3.monitors = 3; m3.primary = 1; m3.selected = 2;
        Dock d3; d3.c.fileMonitor = 0; d3.c.fileAt = 0;   // a file naming another monitor than FluentFlyout uses
        now = 0;
        Outcome o3 = Run(m3, d3, now, 8);
        Check(o3.host == 2 && o3.inHost, "a wrong file is corrected by where FluentFlyout puts the widget");

        // Explorer's taskbar on the main display only: FluentFlyout never finds a taskbar on another
        // monitor and always falls back to the first Shell_TrayWnd (the host window). Nothing but the
        // file tells its monitor then: the dock follows the file, and never goes back and forth.
        bool followsFile = true;
        portable_check::Quiet() = true;
        for (int n = 1; n <= 3; ++n)
            for (int primary = 0; primary < n; ++primary)
                for (int selected = 0; selected < n; ++selected)
                    for (int file = -1; file < n; ++file) {
                        Model mm; mm.monitors = n; mm.primary = primary; mm.selected = selected; mm.secondaries = false;
                        Dock dd; dd.c.fileMonitor = file; dd.c.fileAt = file >= 0 ? 0.0 : -1.0;
                        double t = 0;
                        Outcome oo = Run(mm, dd, t, 10);
                        followsFile &= Check(oo.inHost && oo.host == (file >= 0 ? file : primary) && oo.lateMoves == 0 && oo.moves <= 1,
                                             Label("main display only: the file's monitor, in the host window", n, primary, selected, file).c_str());
                    }
        portable_check::Quiet() = false;
        Check(followsFile, "taskbar on the main display only: the host window goes to the file's monitor and stays");

        // where the widget in one of Explorer's taskbars tells FluentFlyout's monitor
        Check(ShowsMonitor(true, 0, 2) && ShowsMonitor(true, 1, 3), "in a secondary taskbar: always (FluentFlyout takes one only on its monitor)");
        Check(ShowsMonitor(false, 1, 2) && ShowsMonitor(false, 2, 3), "in the main one with a taskbar on every display: the main monitor");
        Check(!ShowsMonitor(false, 0, 2) && !ShowsMonitor(false, 1, 3), "...without one on every display: maybe FluentFlyout's fallback, not an answer");
        Check(ShowsMonitor(false, 0, 1), "one monitor: the only one");

        // The dock starting: its host window is not up, the widget is in Explorer's main taskbar (or a
        // secondary one). With a taskbar on every display that tells FluentFlyout's monitor even against a
        // stale file; without, the file decides - never the main monitor by FluentFlyout's fallback.
        for (bool sec : { true, false }) {
            bool startRight = true;
            portable_check::Quiet() = true;
            for (int n = 1; n <= 3; ++n)
                for (int primary = 0; primary < n; ++primary)
                    for (int selected = 0; selected < n; ++selected)
                        for (int file = -1; file < n; ++file) {
                            Model mm; mm.monitors = n; mm.primary = primary; mm.selected = selected; mm.secondaries = sec;
                            Dock dd; dd.c.fileMonitor = file; dd.c.fileAt = file >= 0 ? 0.0 : -1.0;
                            double t = 0;
                            Outcome oo = RunFromStart(mm, dd, t, 10);
                            const int want = n == 1 ? 0 : sec ? selected : file >= 0 ? file : primary;
                            startRight &= Check(oo.inHost && oo.host == want && oo.lateMoves == 0 && oo.moves == 0,
                                                Label(sec ? "starting, taskbars on all displays: FluentFlyout's monitor at once"
                                                          : "starting, taskbar on the main display only: the file's monitor at once", n, primary, selected, file).c_str());
                        }
            portable_check::Quiet() = false;
            Check(startRight, sec ? "the dock starting, taskbars on all displays: its host window goes to FluentFlyout's monitor, a stale file or not"
                                  : "the dock starting, taskbar on the main display only: the file's monitor, not the main one by FluentFlyout's fallback");
        }
    }

    // ---------------------------------------------------------------- "something is playing"
    {
        Steady s;
        Check(s.value && !s.Waiting(), "unknown counts as playing");
        Check(!s.Update(false, 0.0) && s.value && s.Waiting(), "nothing playing: held back at first");
        Check(!s.Update(true, 0.5) && s.value && !s.Waiting(), "a new session within the moment: no change at all");
        s.Update(false, 1.0);
        Check(!s.Update(false, 2.4) && s.value, "1.4 s without a session: still playing");
        Check(s.Update(false, 2.5) && !s.value, "1.5 s: nothing playing (one change)");
        Check(!s.Update(false, 9.0) && !s.value, "...and no repeated change");
        Check(s.Update(true, 9.1) && s.value, "playing again: at once");

        // a flicker seen in a dock.log: sessions replaced every few seconds, gaps of 40-400 ms
        Steady f;
        const double gaps[][2] = { { 0.0, 0.06 }, { 1.0, 1.2 }, { 2.0, 2.4 }, { 3.0, 3.05 }, { 5.0, 5.3 } };
        int changes = 0;
        for (auto& g : gaps) {
            changes += f.Update(false, g[0]);
            changes += f.Update(false, (g[0] + g[1]) / 2);
            changes += f.Update(true, g[1]);
        }
        Check(changes == 0 && f.value, "sessions replaced with short gaps: the slot never closes");
        Steady longer;
        Check(!longer.Update(false, 0, 0.2) && longer.Update(false, 0.25, 0.2) && !longer.value, "the hold time is a parameter");
    }

    // ---------------------------------------------------------------- waiting for a new widget window
    {
        Check(!StillWaiting(-1, 5), "nothing closed: not waiting");
        Check(StillWaiting(10, 10) && StillWaiting(10, 12.9), "a widget window just closed: its place is kept");
        Check(!StillWaiting(10, 10 + kRehomeSeconds), "after kRehomeSeconds: given up");
    }
    return Summary("fluent_logic_test");
}
