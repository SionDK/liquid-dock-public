#include "apps.h"
#include "apps_internal.h"
#include "admin_client.h"   // Elevated: the log names a window in front that runs as administrator
#include "app_click.h"
#include "system.h"         // TakeForegroundRight
#include "taskbar_apps.h"   // a click through the app's button on the taskbar
#include "explorer_taskbar.h"   // ExplorerTaskbar: asked whether it answers after a press

// ---------------------------------------------------------------- the taskbar's buttons (taskbar_apps.h)

// The click goes to the app's button on the taskbar, looked up and pressed on a worker. Explorer may put a
// window in front then only with the dock's leave: the dock needs that right itself first (shell_front.h) and
// hands it on to Explorer - to Explorer alone: the process named in an AllowSetForegroundWindow loses the right
// at the next such call naming another (Microsoft's documentation), and it is Explorer that brings the window
// up (mod.21 named the app after Explorer, which took Explorer's leave away again).
bool AppModel::StartPress(AppItem& a, const appclick::Decision& d, int dockIndex, HMONITOR monitor, int usable, double now) {
    taskbarapps::Job job;
    job.id = ++pressIds ? pressIds : ++pressIds;   // never 0
    if (!monitor) {
        POINT p{};
        GetCursorPos(&p);
        monitor = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    }
    job.monitor = monitor;
    job.aumid = a.aumid;
    job.exe = a.exe;
    HWND target = (HWND)(uintptr_t)d.window;
    if (target) {   // its button is the window's: named by the window's own id, which may not be the app's (apps grouped by program)
        auto f = winCache.find(target);
        if (f != winCache.end()) {
            if (!f->second.aumid.empty() && _wcsicmp(f->second.aumid.c_str(), a.aumid.c_str())) {
                job.aumid = f->second.aumid;
                job.otherAumid = a.aumid;
            }
            if (!f->second.exe.empty()) job.exe = f->second.exe;
        }
    }
    job.running = d.action != appclick::Action::Launch;
    job.wake = g_mainWnd;
    TakeForegroundRight();
    DWORD shellPid = 0, pid = 0;
    if (target) GetWindowThreadProcessId(target, &pid);
    if (HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &shellPid);
    if (shellPid) AllowSetForegroundWindow(shellPid);
    const HWND before = GetForegroundWindow();   // before the worker runs: its press may change it at once
    DWORD beforePid = 0;
    if (before) GetWindowThreadProcessId(before, &beforePid);
    if (!taskbarapps::Begin(job)) return false;   // no thread to be had: the dock's own way
    Press p;
    p.job = job.id;
    p.stage = job.stage;
    p.aumid = job.aumid;
    p.otherAumid = job.otherAumid;
    p.exe = job.exe;
    p.item = a.id;
    p.d = d;
    p.dockIndex = dockIndex;
    p.usable = usable;
    p.monitor = monitor;
    p.clicked = now;
    p.before = before;
    p.frontWasItsProgram = before && before != target && pid && beforePid == pid;
    presses.push_back(p);
    if (target && d.action == appclick::Action::Activate) QuietFlashes(target, now);   // Windows refusing Explorer flashes it: no attention bounce
    return true;
}

// Do its program and Explorer's taskbar answer (WM_NULL, as Windows asks before calling a program not responding)?
// Explorer too: a press it took may still be on its way while it is busy. On a worker: a busy program would hold
// the asker. 1 both answered; 3 Explorer did not within 0.3 s, 2 the program did not (or no thread to ask on:
// taken for busy, so the dock waits rather than does it twice). A program running as administrator refuses the
// question (error 5): taken for one that answers - that refusal is no sign of a busy program.
static std::shared_ptr<std::atomic<int>> AskWhetherAnswers(HWND h, HWND explorer) {
    auto state = std::make_shared<std::atomic<int>>(0);
    const bool asks = TryDetach([h, explorer, state] {   // handles and a shared_ptr: making the lambda throws nothing
        auto answers = [](HWND w) {
            if (!w) return true;
            DWORD_PTR result = 0;
            return SendMessageTimeoutW(w, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &result) != 0 ||
                   GetLastError() == ERROR_ACCESS_DENIED;
        };
        *state = !answers(explorer) ? 3 : !answers(h) ? 2 : 1;
    });
    if (!asks) *state = 2;
    return state;
}

// A window of the thread that runs a taskbar's buttons (its XAML island), else the taskbar's own; nullptr: the main one.
static BOOL CALLBACK FindIsland(HWND w, LPARAM p) {
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge")) return TRUE;
    *reinterpret_cast<HWND*>(p) = w;
    return FALSE;
}
static HWND TaskbarButtonsWindow(HWND bar) {
    if (!bar || !IsWindow(bar)) bar = ExplorerTaskbar();
    HWND island = nullptr;
    if (bar) EnumChildWindows(bar, FindIsland, reinterpret_cast<LPARAM>(&island));
    return island ? island : bar;
}

// The taskbar's own window in front (a press may bring it there for a moment): not the user going on.
static bool IsTaskbarItself(HWND h) {
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    return !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd");
}

// The app of that AppItem::id still on the dock (nullptr: it left meanwhile).
static std::shared_ptr<AppItem> ItemById(const std::vector<std::shared_ptr<AppItem>>& items, int id) {
    for (auto& it : items) if (it->id == id) return it;
    return nullptr;
}

// The presses' results, as they come in (taskbar_apps_logic.h NextAfter): pressed - the dock watches the window
// (CheckPress), or the launch; surely not pressed - the dock does the click itself, at once; unsure (a timeout) -
// nothing more for that click. A lookup not done in taskbarapps::kLookupWait is abandoned: the dock does the click.
void AppModel::PollPresses(double now) {
    for (size_t i = 0; i < presses.size();) {
        taskbarapps::Result r;
        if (!taskbarapps::Take(presses[i].job, r)) {
            const double waited = now - presses[i].clicked;
            if (waited > taskbarapps::kLookupWait && taskbarapps::Abandon(*presses[i].stage)) {
                r.outcome = taskbarapps::Outcome::Abandoned;   // no press under way yet, and none will come
            } else if (waited > taskbarapps::kPressAnswer) {   // the press itself stuck in a call into Explorer: it may still arrive
                r.outcome = taskbarapps::Outcome::Unsure;
                r.how = L"no answer from Explorer";
                r.hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            } else {
                ++i;
                continue;
            }
        }
        // a copy: the press is gone from presses, and the dock's own way below (ClickWith) may start another
        const Press p = presses[i];
        presses.erase(presses.begin() + i);
        const bool launch = p.d.action == appclick::Action::Launch;
        const taskbarapps::Next next = taskbarapps::NextAfter(r.outcome, launch, r.slow);
        if (next.slow) slowUntil = now + taskbarapps::kSlowPause;
        if (next.off) pressUse.use = taskbarapps::Use::Off;
        const std::shared_ptr<AppItem> a = ItemById(items, p.item);
        if (!a) continue;   // the app left the dock meanwhile
        if (next.noButton) noButtonUntil[a->id] = now + taskbarapps::kNoButtonAgain;
        SayPressOutcome(*a, p, r, next, launch, now);
        HandOnPress(*a, p, r, next, now);
    }
}

// The press's result in dock.log (PollPresses): each app's outcome once a run, and again when it changes - but a
// first or slow press, and every press that may not have arrived.
void AppModel::SayPressOutcome(const AppItem& a, const Press& p, const taskbarapps::Result& r, const taskbarapps::Next& next,
                               bool launch, double now) {
    int& said = saidPress[a.id];
    const bool sayIt = said != (int)r.outcome + 1;   // each app's outcome once a run (and again when it changes)
    said = (int)r.outcome + 1;
    switch (r.outcome) {
    case taskbarapps::Outcome::Pressed:
        if (!saidFirstPress || r.lookup + r.took >= 0.3) {
            saidFirstPress = true;
            Log(L"app click: %ls - pressed its button on the taskbar (%ls; found in %.0f ms among %d app buttons, pressed in %.0f ms)",
                a.name.c_str(), r.how, r.lookup * 1000, r.buttons, r.took * 1000);
        }
        break;
    case taskbarapps::Outcome::Unsure:
        Log(L"app click: %ls - unsure whether its taskbar button's press arrived (%ls, %08lx): nothing more sent for that click, "
            L"and %ls for up to %.0f s", a.name.c_str(), r.how, (unsigned long)r.hr,
            launch ? L"no second start" : L"nothing on top of it", launch ? taskbarapps::kLaunchCheck : appclick::kBusyWait);
        break;
    case taskbarapps::Outcome::Abandoned:
        if (sayIt)
            Log(L"app click: %ls - its taskbar button was not found and pressed within %.1f s (Explorer slow to answer): the dock does "
                L"it itself, and every app click for %.0f s", a.name.c_str(), now - p.clicked, taskbarapps::kSlowPause);
        break;
    case taskbarapps::Outcome::Classic:
        if (sayIt) Log(L"app click: Windows' classic taskbar has no app buttons to press: the dock's own way for this run");
        break;
    case taskbarapps::Outcome::NoWay:
        if (sayIt)
            Log(L"app click: %ls - its taskbar button was found (in %.0f ms among %d app buttons) but not pressed: %ls. The "
                L"dock does it itself, and goes its own way for the rest of this run", a.name.c_str(), r.lookup * 1000, r.buttons, r.how);
        break;
    case taskbarapps::Outcome::NoButton:
        if (sayIt) {
            std::wstring as;
            for (const std::wstring* id : { &p.aumid, &p.otherAumid })
                if (!id->empty()) as += (as.empty() ? L"id " : L" or id ") + *id;
            if (!p.exe.empty()) as += (as.empty() ? L"path " : L" or path ") + p.exe;
            Log(L"app click: %ls - %ls (it was looked for as %ls; %d app buttons there): the dock does it itself, and looks again "
                L"after %.0f s", a.name.c_str(), taskbarapps::OutcomeText(r.outcome), as.c_str(), r.buttons, taskbarapps::kNoButtonAgain);
        }
        break;
    case taskbarapps::Outcome::CountDiffers:
        if (sayIt)
            Log(L"app click: %ls - its taskbar button counts %ls, the dock %d: the dock does it itself, and looks again after %.0f s",
                a.name.c_str(), r.count < 0 ? L"no window (a pinned button)" : (std::to_wstring(r.count) + L" windows").c_str(),
                launch ? 0 : p.usable, taskbarapps::kNoButtonAgain);
        break;
    case taskbarapps::Outcome::Failed:
    case taskbarapps::Outcome::NoTaskbar:
        if (sayIt)
            Log(L"app click: %ls - its taskbar button: %ls (%ls, %08lx): the dock does it itself%ls", a.name.c_str(),
                taskbarapps::OutcomeText(r.outcome), r.how, (unsigned long)r.hr,
                next.slow ? (L", and every app click for " + std::to_wstring((int)taskbarapps::kSlowPause) + L" s").c_str() : L"");
        break;
    }
}

// What follows the press's result (PollPresses, NextAfter): the launch looked at, the window watched (CheckPress),
// or the click done the dock's own way. p: PollPresses' copy (ClickWith may add to presses).
void AppModel::HandOnPress(AppItem& a, const Press& p, const taskbarapps::Result& r, const taskbarapps::Next& next, double now) {
    if (next.launched) {   // a pinned app's button starts it: the dock's launch bounce, and a look whether it came
        a.launchStarted = now;
        a.launchDock = p.dockIndex;
        a.launchWindowCount = a.windows.size();
        launchChecks.push_back({ a.id, now, a.windows.size(), next.maybe });
    } else if (next.watch) {
        const HWND h = (HWND)(uintptr_t)p.d.window;
        if (next.maybe)
            maybes.push_back({ h, p.clicked + appclick::kBusyWait, p.d.action == appclick::Action::Minimize, p.d.why == appclick::Why::Minimized });
        // a window clicked after this one is being watched already (the dock's own way): that watch stays -
        // this press goes unchecked (one that may still arrive is kept off by maybes all the same)
        if (watch.window && watch.window != h && watch.clicked > p.clicked) return;
        NewWatch(h, p.before, a.name, appclick::WhyText(p.d.why), p.usable, p.clicked);
        watch.clicks = p.clicks;
        watch.byTaskbar = true;
        watch.wantDown = p.d.action == appclick::Action::Minimize;
        watch.downAtClick = p.d.why == appclick::Why::Minimized;
        watch.explorerBusy = r.took >= taskbarapps::kExplorerBusy || next.maybe;   // maybe: whatever comes, comes late - never a dud
        watch.frontWasItsProgram = p.frontWasItsProgram;
        watch.taskbar = r.taskbar;
        watch.pressedAt = next.maybe ? p.clicked : now;   // maybe: kBusyWait counts from the click
        watch.next = now + appclick::kLook;
    } else if (next.itself) {
        ClickWith(a, p.dockIndex, p.monitor, false);   // the dock's own way, the click's decision taken anew
    }
}

// An app left the dock: its id is never given again (NewItem), so what the presses kept under it is done with.
void AppModel::ForgetPresses(int id) {
    noButtonUntil.erase(id);
    saidPress.erase(id);
}

// What CheckPress sees of the pressed window h now (taskbar_apps_logic.h Seen, for Judge).
taskbarapps::Seen AppModel::LookAtPress(HWND h, double now) const {
    const Watch& w = watch;
    taskbarapps::Seen s;
    s.since = now - w.pressedAt;
    s.wantDown = w.wantDown;
    s.downAtClick = w.downAtClick;
    s.gone = !IsWindow(h);
    const HWND fg = GetForegroundWindow();
    DWORD pid = 0, fgPid = 0;
    if (!s.gone) GetWindowThreadProcessId(h, &pid);
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    const bool shown = !s.gone && IsWindowVisible(h) && !IsCloaked(h);
    s.minimized = !s.gone && IsIconic(h);
    s.hidden = !s.gone && !shown;
    // another window of its program was in front at the click (a browser profile, a store app's frame): that
    // one still in front is no sign of the press - only this window (or one it owns) coming counts then
    const bool isIt = IsItOrItsOwn(fg, h);
    s.cameUp = !s.gone && appclick::CameUp(shown, s.minimized, isIt, !w.frontWasItsProgram && fg && pid && fgPid == pid);
    s.elsewhere = fg && fg != w.before && fgPid != pid && !IsTaskbarItself(fg);
    s.answers = w.answers ? w.answers->load() : -1;
    s.explorerBusy = w.explorerBusy;
    return s;
}

// The window the taskbar's press brings up or down: the dock looks (taskbar_apps_logic.h, Judge) and steps in
// only when the press did nothing while the window's program answered.
taskbarapps::Verdict AppModel::CheckPress(double now) {
    Watch& w = watch;
    const HWND h = w.window;
    const taskbarapps::Seen s = LookAtPress(h, now);
    const taskbarapps::Verdict v = taskbarapps::Judge(s);
    switch (v) {
    case taskbarapps::Verdict::Wait:
        break;
    case taskbarapps::Verdict::Ask:
        w.answers = AskWhetherAnswers(h, TaskbarButtonsWindow(w.taskbar));
        break;
    case taskbarapps::Verdict::Done:
        if (!w.wantDown) SawCameUp(h, now);
        if (!w.forced && taskbarapps::Learn(pressUse, v))   // forced: the dock's own doing, maybe
            Log(L"app click: the taskbar's app buttons work: apps with one window, and pinned apps not running, go through them from now on");
        if (now - w.clicked > 1.0)
            Log(L"app click: %ls %ls after %.1f s, by its taskbar button%ls", w.name.c_str(), w.wantDown ? L"went down" : L"came up",
                now - w.clicked, w.explorerBusy ? L" (Explorer was busy)" : L"");
        EndPressWatch(h);
        return v;
    case taskbarapps::Verdict::Toggled:   // Windows minimized it: the press arrived, and Windows' doing stands
        if (taskbarapps::Learn(pressUse, v))
            Log(L"app click: the taskbar's app buttons work: apps with one window, and pinned apps not running, go through them from now on");
        Log(L"app click: %ls - its taskbar button minimized it instead of bringing it up (Windows took it for the active app; in "
            L"front: %ls): left so", w.name.c_str(), InFront().c_str());
        EndPressWatch(h);
        return v;
    case taskbarapps::Verdict::Behind: {   // restored, but Windows gave it no front: the press arrived; it is up - no restore of the dock's
        // Explorer busy: the wait may have cost it the right to the front - nothing learnt of that
        const bool turnedOff = !w.forced && !w.explorerBusy && taskbarapps::Learn(pressUse, v) && pressUse.use == taskbarapps::Use::Off;
        Log(L"app click: %ls - its taskbar button restored it, but Windows did not put it in front in %.1f s (in front: %ls%ls): the "
            L"dock does that itself%ls", w.name.c_str(), s.since, InFront().c_str(), w.explorerBusy ? L"; Explorer was busy" : L"",
            turnedOff ? L"; apps go the dock's own way for the rest of this run" : L"");
        const wchar_t* why = w.why;
        const int windows = w.windows;
        EndPressWatch(h);
        BringUp(h, why, windows);   // not minimized: no restore command, only the dock's front window calls (and its watch)
        return v;
    }
    case taskbarapps::Verdict::Stop:
        EndPressWatch(h);
        return v;
    case taskbarapps::Verdict::GiveUp:
        Log(L"app click: %ls - its taskbar button was pressed and it did not %ls in %.1f s (%ls): nothing more sent (window: %ls; in front: %ls)",
            w.name.c_str(), w.wantDown ? L"go down" : L"come up", s.since,
            w.explorerBusy || s.answers == 3 ? L"Explorer was busy" : L"its program does not answer", Describe(h).c_str(), InFront().c_str());
        if (w.explorerBusy || s.answers == 3) slowUntil = now + taskbarapps::kSlowPause;
        EndPressWatch(h);
        return v;
    case taskbarapps::Verdict::Dud: {
        const bool turnedOff = taskbarapps::Learn(pressUse, v) && pressUse.use == taskbarapps::Use::Off;
        Log(L"app click: %ls - its taskbar button was pressed and nothing happened in %.1f s while its program answers: the dock "
            L"does it itself%ls", w.name.c_str(), s.since, turnedOff ? L"; apps go the dock's own way for the rest of this run" : L"");
        const bool down = w.wantDown;
        const std::wstring name = w.name;
        const wchar_t* why = w.why;
        const int windows = w.windows;
        watch = {};
        if (down) {
            MinimizeWindow(h, name);
        } else {
            BringUp(h, why, windows);
        }
        return v;
    }
    }
    w.next = now + appclick::kLook;
    return v;
}

bool AppModel::MaybeOnItsWay(HWND h, double now) {
    for (size_t i = 0; i < maybes.size();) {
        const Maybe& m = maybes[i];
        // it showed: down as wanted, up in front as wanted, restored (in front or not: CheckPress, Behind) - or down
        // although it was up (Windows took it for a minimize)
        const bool down = IsIconic(m.window) || !IsWindowVisible(m.window);
        const bool shown = IsWindow(m.window) &&
                           (m.wantDown ? down : (!down && (m.downAtClick || GetForegroundWindow() == m.window)) || (!m.downAtClick && down));
        if (now >= m.until || !IsWindow(m.window) || shown) { maybes.erase(maybes.begin() + i); continue; }   // over
        if (m.window == h) return true;
        ++i;
    }
    return false;
}

void AppModel::ForgetMaybe(HWND h) {
    maybes.erase(std::remove_if(maybes.begin(), maybes.end(), [h](const Maybe& m) { return m.window == h; }), maybes.end());
}

// The press's watch over with a verdict on it (CheckPress): a press for it no longer on its way. Not for a dud: a
// maybe left then holds the dock's own bring-up back, as it always has (a state not met today: a maybe is
// explorerBusy, never a dud). h by value: watch.window is cleared here.
void AppModel::EndPressWatch(HWND h) {
    ForgetMaybe(h);
    watch = {};
}

// Pinned apps started by their taskbar button: a window of theirs within taskbarapps::kLaunchCheck, or a miss.
// Nothing is started again for a miss (the app may only be slow); two in a row and the dock starts pinned
// apps itself for the rest of the run.
void AppModel::CheckLaunches(double now) {
    for (size_t i = 0; i < launchChecks.size();) {
        const LaunchCheck c = launchChecks[i];
        const std::shared_ptr<AppItem> a = ItemById(items, c.item);
        const bool came = a && a->windows.size() > c.windows;
        if (a && !came && now - c.at < taskbarapps::kLaunchCheck) { ++i; continue; }
        launchChecks.erase(launchChecks.begin() + i);
        if (!a) continue;   // the app left the dock meanwhile
        const bool off = taskbarapps::LearnLaunch(launchUse, came, c.maybe);
        if (!came)
            Log(L"app click: %ls - its pinned taskbar button was pressed%ls and no window of it came in %.0f s (nothing started again)%ls",
                a->name.c_str(), c.maybe ? L" (the press may not have arrived)" : L"", taskbarapps::kLaunchCheck,
                off ? L"; the dock starts pinned apps itself for the rest of this run" : L"");
    }
}
