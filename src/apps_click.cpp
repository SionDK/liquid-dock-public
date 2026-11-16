#include "apps.h"
#include "apps_internal.h"
#include "admin_helper.h"
#include "admin_client.h"   // Elevated: the log names a window in front that runs as administrator
#include "app_click.h"
#include "system.h"         // TakeForegroundRight
#include "taskbar_apps.h"   // a click through the app's button on the taskbar
#include "explorer_taskbar.h"   // ExplorerTaskbar: asked whether it answers after a press
#include "gfx.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <fstream>
#include <sstream>
#include <thread>

// ---------------------------------------------------------------- actions

// A program run as administrator: Windows refuses these calls to the dock (it runs as the user), so the
// optional elevated helper does them (admin_helper.h). Every other window: the dock itself.
static std::wstring AppNameOf(AppModel& m, HWND h) {
    auto a = m.FindByWindow(h);
    return a ? a->name : std::wstring(L"이 앱");
}

// The calls that put a window in front, when Windows does not take SetForegroundWindow alone. None of
// them waits for the window's program: BringWindowToTop did (it sends to the window and waits), so a
// busy program held the dock up while it was clicked. Returns why attaching to the window in front
// failed (GetLastError; 0: it was not needed, or it worked), for the log.
static unsigned long ForceForeground(HWND h) {
    if (SetForegroundWindow(h) && GetForegroundWindow() == h) return 0;
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    const bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    const unsigned long attachError = fgThread && fgThread != me && !attached ? GetLastError() : 0;
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    SetForegroundWindow(h);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
    return attachError;
}

// A minimized window's restore command, as from Explorer's taskbar (its program restores it the way it
// knows: maximized again, say). Sent on a worker thread - app_click.h says why - so the dock learns
// when the program handled it; one that handled it and left the window down gets the system's restore.
// No thread to be had (the system out of memory): the system's restore at once, which waits on nothing -
// one restore all the same, and the click still brings the window up (its program's own way is skipped).
static std::shared_ptr<RestoreJob> StartRestore(HWND h) {
    auto job = std::make_shared<RestoreJob>();
    try {
        std::thread([h, job] {
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(h, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_NORMAL, (UINT)(appclick::kBusyWait * 1000), &result)) {
                for (int i = 0; i < 6 && IsIconic(h); ++i) Sleep(25);   // a program that restores a moment later
                if (IsIconic(h)) {   // handled, still down; nothing of the dock's is queued for it now: no double restore
                    job->ignored = true;
                    ShowWindowAsync(h, SW_RESTORE);
                }
                job->handledAt = NowSeconds();
                job->state = 1;
            } else {
                job->error = GetLastError();   // 1460: no answer in time; 5: refused (an elevated program)
                job->handledAt = NowSeconds();
                job->state = 2;
            }
        }).detach();
    } catch (...) {
        Log(L"app click: no thread to send the restore command on: the system's restore instead");
        ShowWindowAsync(h, SW_RESTORE);
        job->handledAt = NowSeconds();
        job->state = 1;
    }
    return job;
}

// A window as the log describes it: kind, size, state and the styles that decide how it behaves (never
// its title).
std::wstring Describe(HWND h) {
    if (!IsWindow(h)) return L"gone";
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    RECT r{};
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (IsIconic(h) && GetWindowPlacement(h, &wp)) r = wp.rcNormalPosition;   // its size when restored
    else GetWindowRect(h, &r);
    wchar_t size[40];
    swprintf_s(size, L" %ldx%ld", r.right - r.left, r.bottom - r.top);
    std::wstring s = std::wstring(cls) + size;
    const LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE), ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (IsIconic(h)) s += L" minimized";
    else if (IsZoomed(h)) s += L" maximized";
    if (!IsWindowVisible(h)) s += L" hidden";
    if (IsCloaked(h)) s += L" cloaked";
    if (!IsWindowEnabled(h)) s += L" disabled";
    if (ex & WS_EX_TOPMOST) s += L" topmost";
    if (ex & WS_EX_LAYERED) s += L" layered";
    if (ex & WS_EX_TOOLWINDOW) s += L" tool";
    if (ex & WS_EX_NOACTIVATE) s += L" no-activate";
    if (GetWindow(h, GW_OWNER)) s += L" owned";
    if (!(st & WS_CAPTION)) s += L" no-caption";
    if (IsHungAppWindow(h)) s += L" not-responding";
    return s;
}

// The program and window class in front, for the log.
std::wstring InFront() {
    HWND fg = GetForegroundWindow();
    if (!fg) return L"nothing";
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    wchar_t cls[96] = {};
    GetClassNameW(fg, cls, ARRAYSIZE(cls));
    std::wstring exe = ProcessExe(pid);
    size_t slash = exe.find_last_of(L'\\');
    return (slash == std::wstring::npos ? exe : exe.substr(slash + 1)) + L" " + cls +
           (!IsWindowVisible(fg) ? L" hidden" : IsCloaked(fg) ? L" cloaked" : L"") +   // in front with nothing on the screen
           (adminhelper::Elevated(fg) ? L" (administrator)" : L"");
}

std::shared_ptr<RestoreJob> AppModel::SendRestore(HWND h) {
    auto job = StartRestore(h);
    restores.push_back({ h, job });
    return job;
}

// The restore command for this window that is still on its way to its program, or was handled a moment ago (the
// system's restore may still follow), whichever watch sent it; the rest are forgotten here. One not delivered is
// no longer on its way, nor one handled before the window was seen come up (ForgetHandledRestores).
std::shared_ptr<RestoreJob> AppModel::RestoreUnderWay(HWND h, double now) {
    std::shared_ptr<RestoreJob> found;
    for (size_t i = 0; i < restores.size();) {
        const HWND w = restores[i].first;
        const std::shared_ptr<RestoreJob> job = restores[i].second;
        const int state = job->state;
        if (state != 0 && now - job->handledAt >= appclick::kAfterRestore) { restores.erase(restores.begin() + i); continue; }
        if (w == h && state != 2) found = job;
        ++i;
    }
    return found;
}

// The window was seen come up: a command for it its program handled has done its part (the system's restore after
// it too), and a click on it minimized again meanwhile is a new wish - a restore of its own. One its program has not
// answered stays: it may still arrive.
void AppModel::ForgetHandledRestores(HWND h) {
    restores.erase(std::remove_if(restores.begin(), restores.end(), [h](const auto& r) { return r.first == h && r.second->state != 0; }),
                   restores.end());
}

void AppModel::ActivateWindow(HWND h) { BringUp(h, L"chosen from its window list", 0); }

void AppModel::BringUp(HWND h, const wchar_t* why, int windows) {
    if (MaybeOnItsWay(h, NowSeconds())) {   // a press for it may still arrive (Explorer busy): nothing on top of it
        if (watch.window == h) ++watch.clicks;
        return;
    }
    // its taskbar button being looked up for it (a click on the dock a moment ago): no press then - the dock
    // brings it up itself; a press under way already: nothing on top of it (its watch will follow)
    for (size_t i = 0; i < presses.size(); ++i)
        if ((HWND)(uintptr_t)presses[i].d.window == h) {
            if (!taskbarapps::Abandon(*presses[i].stage)) { ++presses[i].clicks; return; }
            presses.erase(presses.begin() + i);
            break;
        }
    if (watch.byTaskbar && watch.window == h) {
        // the taskbar's press is bringing it up: nothing on top of it (a second restore would take a window
        // coming back maximized out of maximized); the press does nothing - the watch brings it up itself
        if (!watch.wantDown) { ++watch.clicks; return; }
        watch = {};   // the press was minimizing it: that is over, it is wanted up now
    }
    if (watch.window == h && watch.popup) { ++watch.clicks; return; }   // clicked again while Start or Search closes: the same wait
    const std::wstring name = AppNameOf(*this, h);
    // The right to change the front window first: a click on the dock's own window does not always give
    // it (system.h), and the admin helper below gets it from the dock.
    const ForegroundRight right = TakeForegroundRight();
    const double now = NowSeconds();
    QuietFlashes(h, now);
    // Start or Search in front: closed first; the rest once it has gone (app_click.h NextForPopup, CheckWatch). Only one
    // that shows: a closed one stays a cloaked window of its program, and can keep the front with nothing on the
    // screen - SearchHost's after the monitors came back on (2026-10-30: six clicks on Chrome
    // dropped, no Esc ever sent to it). Nothing to close there: the window is brought up as usual.
    const HWND front = GetForegroundWindow();
    const bool shellFront = front && front != h && IsShellPopupWindow(front);
    if (shellFront && !IsCloaked(front)) {
        watch = {};
        watch.window = h;
        watch.before = front;
        watch.name = name;
        watch.why = why;
        watch.windows = windows;
        watch.clicked = now;
        watch.restore = RestoreUnderWay(h, now);
        watch.right = watch.rightAtClick = (int)right;
        watch.popup = front;
        watch.hadPopup = true;
        ClosePopup(now);
        watch.next = now + appclick::kPopupLook;
        return;
    }
    if (adminhelper::Activate(h, name)) return;
    if (watch.window == h) {
        ++watch.clicks;   // clicked again while it is coming: the same wait goes on
    } else {
        watch = {};
        watch.window = h;
        watch.before = front;
        watch.name = name;
        watch.why = why;
        watch.windows = windows;
        watch.clicked = now;
        watch.restore = RestoreUnderWay(h, now);   // an earlier watch's command for it, its program busy still: waited for
        watch.rightAtClick = (int)right;
        if (shellFront) {
            watch.ghostFront = true;
            Log(L"app click: %ls - %ls is in front but not shown (a closed Start or Search keeping the front): no Esc, "
                L"bringing the window up as usual", name.c_str(), InFront().c_str());
        }
    }
    watch.right = (int)right;
    Push(now, true);
    watch.next = now + appclick::kLook;
}

// The watched window put in front. At the click (or once the pop-up in front at the click has gone): its program
// may take the front itself as it restores, and a minimized one gets its restore command - one at a time: none
// while its program has not answered the last, none right after it did (the system's restore may still be on its
// way). Then the calls that put it in front: the dock's own, or the admin helper's while a window that runs as
// administrator is in front - Windows does not let the dock (it runs as the user) take the front from such a
// window: its key for the right is dropped there, attaching to it is refused, and SetForegroundWindow only
// flashes the window, the right held or not (2026-10-29: OBS run as administrator). A window
// still minimized waits for the helper until it is up (the next look): the helper restores what it brings up,
// and a second restore would take a window coming back maximized out of maximized.
void AppModel::Push(double now, bool atClick) {
    Watch& w = watch;
    const HWND h = w.window;
    if (atClick) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid) AllowSetForegroundWindow(pid);
        const auto& job = w.restore;
        const bool unanswered = job && job->state == 0, justHandled = job && job->state != 0 && now - job->handledAt < appclick::kAfterRestore;
        if (IsIconic(h) && !unanswered && !justHandled) w.restore = SendRestore(h);
    }
    const HWND fg = GetForegroundWindow();
    if (fg && fg != h && !IsIconic(h) && adminhelper::Elevated(fg) && adminhelper::ActivateOver(h, w.name)) {
        ++w.overAdmin;
        // its answer comes after its calls: in front now, or the helper could not (one older than mod.24 has only
        // the right the dock passed on) - the dock's own calls then, which at least raise the window
        const HWND front = GetForegroundWindow();
        if (front == h || (front && GetAncestor(front, GA_ROOTOWNER) == h)) return;
    }
    w.attachError = ForceForeground(h);
}

// Esc to the shell pop-up in front at the click - only while it is still in front and shown (not on its way out): a
// key for the window behind it would be a key in that program (a game's menu). Esc is also input of the dock's own:
// the right to change the front window for the window it brings up next. A try counts whether or not the key went
// (kPopupTries); popupEsc, for the log, only the keys that did.
void AppModel::ClosePopup(double now) {
    ++watch.popupClosed;
    watch.popupClosedAt = now;
    const HWND p = watch.popup;
    if (GetForegroundWindow() == p && IsWindowVisible(p) && !IsCloaked(p) && SendKeyCombo({ VK_ESCAPE })) ++watch.popupEsc;
}

void AppModel::QuietFlashes(HWND h, double now) {
    quietFlashes.erase(std::remove_if(quietFlashes.begin(), quietFlashes.end(),
                                      [&](const QuietFlash& q) { return now >= q.until || q.window == h; }),
                       quietFlashes.end());
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    quietFlashes.push_back({ h, pid, now + appclick::kQuietFlash });
}

bool AppModel::FlashQuiet(HWND h, double now) {
    DWORD pid = 0;
    bool asked = false;
    for (const QuietFlash& q : quietFlashes) {
        if (now >= q.until) continue;
        if (q.window == h) return true;
        if (!asked) { GetWindowThreadProcessId(h, &pid); asked = true; }
        if (pid && q.pid == pid) return true;
    }
    return false;
}

// The window being brought up, looked at every appclick::kLook until it came (app_click.h, Next): a
// busy program is waited for, a window up but behind another is put in front again, and the log says
// what happened when it took long or did not come.
void AppModel::CheckWatch(double now) {
    if (watch.byTaskbar) { CheckPress(now); return; }
    Watch& w = watch;
    const HWND h = w.window;
    if (w.popup) {   // Start or Search was in front at the click: closing (BringUp)
        if (!IsWindow(h)) { watch = {}; return; }
        const HWND fg = GetForegroundWindow();
        // the shell may hand the front on through its own windows (Start's after Search's) or through none for a
        // moment: that is not the window the pop-up goes back to. One not shown (cloaked: closed, or on its way
        // out) has nothing to close: gone, as one out of the front (BringUp).
        const bool shown = fg && !IsCloaked(fg);
        if (fg && fg != w.popup && shown && IsShellPopupWindow(fg)) w.popup = fg;
        if (!fg && now - w.popupClosedAt < appclick::kPopupGone) { w.next = now + appclick::kPopupLook; return; }
        switch (appclick::NextForPopup(fg && fg == w.popup && shown, w.popupClosed, now - w.popupClosedAt)) {
        case appclick::PopupStep::Wait:
            w.next = now + appclick::kPopupLook;
            return;
        case appclick::PopupStep::Close:
            ClosePopup(now);
            w.next = now + appclick::kPopupLook;
            return;
        case appclick::PopupStep::GiveUp:
            Log(L"app click: could not bring up %ls - %ls stayed in front after Esc %d times%ls (%.1f s after the click)",
                w.name.c_str(), InFront().c_str(), w.popupEsc,
                w.popupEsc < w.popupClosed ? L" (not sent at the other tries: it had gone from the front or was not shown by then)" : L"",
                now - w.clicked);
            watch = {};
            return;
        case appclick::PopupStep::Gone:
            break;
        }
        // gone: the click goes on from here, the window the shell gave the front back to taken as the one in front
        // at the click (that one staying is no sign it came, and its coming is not the user going elsewhere)
        w.popup = nullptr;
        w.before = fg;
        if (adminhelper::Activate(h, w.name)) { watch = {}; return; }
        w.right = (int)TakeForegroundRight();
        QuietFlashes(h, now);
        Push(now, true);
        w.next = now + appclick::kLook;
        return;
    }
    appclick::Look l;
    l.since = now - w.clicked;
    l.gone = !IsWindow(h);
    HWND fg = GetForegroundWindow();
    DWORD pid = 0, fgPid = 0;
    if (!l.gone) GetWindowThreadProcessId(h, &pid);
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    l.shown = !l.gone && IsWindowVisible(h) && !IsCloaked(h);
    l.minimized = !l.gone && IsIconic(h);
    // a window of its program counts unless it was in front at the click already (another of the dock's icons may
    // be the same program: a browser profile, a store app's frame - as CheckPress); one it owns always does
    const bool isIt = fg && (fg == h || GetAncestor(fg, GA_ROOTOWNER) == h);
    l.cameUp = !l.gone && appclick::CameUp(l.shown, l.minimized, isIt, fg && pid && fgPid == pid, fg == w.before);
    l.elsewhere = fg && fg != w.before && fgPid != pid;
    // the closed Start or Search in front at the click shown again: the user opened it (the shell keeps the one window)
    if (w.ghostFront && fg && fg == w.before && !IsCloaked(fg)) l.elsewhere = true;
    if (w.restore) {
        const int state = w.restore->state;
        l.restoring = state == 0;
        l.failed = state == 2;
        if (state == 1) l.sinceHandled = now - w.restore->handledAt;
    }
    l.refocused = w.refocused;
    switch (appclick::Next(l)) {
    case appclick::Step::Done: {
        lastUp.window = h; lastUp.at = now;
        ForgetHandledRestores(h);
        const bool ignored = w.restore && w.restore->ignored;
        if (l.since > 1.0 || w.saidBusy || ignored || w.ghostFront)
            Log(L"app click: %ls came up after %.1f s%ls", w.name.c_str(), l.since,
                ignored ? L" (its program left the restore command undone: the system's restore brought it)"
                        : w.saidBusy ? L" (its program was busy)"
                        : w.ghostFront ? L" (over the closed Start or Search that kept the front)" : L"");
        watch = {};
        return;
    }
    case appclick::Step::Stop:
        if (!l.gone && (l.minimized || !l.shown))   // it had not come: what came instead
            Log(L"app click: %ls - %ls came in front %.1f s after the click, before it came up", w.name.c_str(), InFront().c_str(), l.since);
        watch = {};
        return;
    case appclick::Step::Wait:
        if (l.restoring && !w.saidBusy && l.since >= appclick::kBusyNote) {
            w.saidBusy = true;
            Log(L"app click: %ls is busy - its program has not answered the restore command in %.1f s (%ls); waiting up to %.0f s",
                w.name.c_str(), l.since, IsHungAppWindow(h) ? L"Windows calls it not responding" : L"Windows still calls it responding",
                appclick::kBusyWait);
        }
        break;
    case appclick::Step::Restore:
        w.restore = SendRestore(h);
        break;
    case appclick::Step::Refocus:
        ++w.refocused;
        w.right = (int)TakeForegroundRight();   // not right after a click any more: take the right again if it is gone
        QuietFlashes(h, now);
        Push(now, false);
        break;
    case appclick::Step::GiveUp: {
        std::wstring what = l.restoring ? L"its program did not answer the restore command"
                          : l.failed ? L"the restore command was not delivered (error " + std::to_wstring(w.restore->error.load()) + L")"
                          : l.minimized ? L"still minimized after its program handled the restore command"
                          : !l.shown ? L"hidden" : L"up, but another window stays in front";
        wchar_t attach[64] = L"";
        if (w.attachError) _snwprintf_s(attach, ARRAYSIZE(attach), _TRUNCATE, L"; attaching to it failed (%lu)", w.attachError);
        std::wstring more;
        if (w.rightAtClick != w.right) more += std::wstring(L" (at the click: ") + ForegroundRightText((ForegroundRight)w.rightAtClick) + L")";
        more += attach;
        if (w.hadPopup) more += L"; Start or Search was in front at the click: closed with Esc";
        if (w.ghostFront) more += L"; a closed Start or Search kept the front at the click (not shown: no Esc)";
        if (w.overAdmin) more += L"; handed to the admin helper " + std::to_wstring(w.overAdmin) + L" times (an administrator's window in front)";
        else if (fg && fg != h && adminhelper::Elevated(fg))
            more += L"; Windows does not let the dock take the front from a window that runs as administrator - the admin helper can "
                    L"(settings: admin app windows)";
        Log(L"app click: could not bring up %ls in %.1f s - %ls (window: %ls; clicked %d times; %ls; %d windows; in front: %ls; "
            L"the dock's right to change the front window: %ls%ls)",
            w.name.c_str(), l.since, what.c_str(), Describe(h).c_str(), w.clicks, w.why, w.windows, InFront().c_str(),
            ForegroundRightText((ForegroundRight)w.right), more.c_str());
        watch = {};
        return;
    }
    }
    w.next = now + appclick::kLook;
}

void AppModel::Launch(AppItem& a, int dockIndex, bool elevated) {
    std::wstring target = a.launch;
    if (target.empty()) target = !a.aumid.empty() ? L"shell:AppsFolder\\" + a.aumid : a.exe;
    if (EndsWithI(target, L".lnk") && GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES && !a.exe.empty())
        target = a.exe;
    if (target.empty()) return;
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_ASYNCOK | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = target.c_str();
    if (elevated && target.rfind(L"shell:", 0) != 0) sei.lpVerb = L"runas";   // Windows shows its own consent prompt
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        a.launchStarted = NowSeconds();
        a.launchDock = dockIndex;
        a.launchWindowCount = a.windows.size();
    } else Log(L"launch failed %s (%lu)", target.c_str(), GetLastError());
}

// One of the app's windows as the click sees it (app_click.h decides).
static appclick::Win ClickFacts(HWND h) {
    appclick::Win w;
    w.id = (uint64_t)(uintptr_t)h;
    w.shown = IsWindow(h) && IsWindowVisible(h) && !IsCloaked(h);
    w.minimized = IsIconic(h) != FALSE;
    RECT r{};
    w.onScreen = !w.minimized && GetWindowRect(h, &r) && r.right - r.left > 1 && r.bottom - r.top > 1 &&
                 MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    w.clickThrough = (ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT);
    return w;
}

void AppModel::Click(AppItem& a, int dockIndex, HMONITOR monitor) {
    // its button on the taskbar is being looked up and pressed for an earlier click: this one only counts
    // (never a second press, nor the dock's own way on top of the press)
    for (auto& p : presses)
        if (p.item == a.id) { ++p.clicks; a.attention = false; return; }
    ClickWith(a, dockIndex, monitor, true);
}

// taskbar: the click may go to the app's button on the taskbar (taskbar_apps_logic.h, ByTaskbar); false when
// the dock does a click the taskbar's way could not.
void AppModel::ClickWith(AppItem& a, int dockIndex, HMONITOR monitor, bool taskbar) {
    std::vector<appclick::Win> wins;
    wins.reserve(a.windows.size());
    for (HWND h : a.windows) wins.push_back(ClickFacts(h));
    const double now = NowSeconds();
    auto mine = [&](HWND h) { return h && std::find(a.windows.begin(), a.windows.end(), h) != a.windows.end(); };
    // the taskbar's press for one of its windows looked at now: one gone down or come up meanwhile is done
    // with, and this click a new wish (a quick second click restores a window the first one minimized). A
    // press found to have done nothing (or to have restored it behind another window) is done by the dock now:
    // this click is taken for that one again.
    if (watch.byTaskbar && mine(watch.window)) {
        const taskbarapps::Verdict v = CheckPress(now);
        if (v == taskbarapps::Verdict::Dud || v == taskbarapps::Verdict::Behind) { a.attention = false; return; }
    }
    // a window of this app still coming, or one that came a moment ago: the click is not a new wish
    const uint64_t pending = mine(watch.window) ? (uint64_t)(uintptr_t)watch.window : 0;
    const uint64_t justUp = mine(lastUp.window) && now - lastUp.at < appclick::kJustUp ? (uint64_t)(uintptr_t)lastUp.window : 0;
    const appclick::Decision d = appclick::Decide(wins, (uint64_t)(uintptr_t)GetForegroundWindow(), pending, justUp);
    HWND target = (HWND)(uintptr_t)d.window;
    int usable = 0;
    for (const auto& w : wins) usable += w.shown && !w.clickThrough;
    // a press for that window may still arrive (Explorer busy): nothing on top of it
    if (target && MaybeOnItsWay(target, now)) {
        if (watch.window == target) ++watch.clicks;
        a.attention = false;
        return;
    }
    // the taskbar's press is bringing its window up or down: a click meanwhile adds no command - a restore
    // of the dock's own on top of the taskbar's would take a window coming back maximized out of maximized.
    // A window already up but not yet in front is put there again (as the dock's own way does at a click).
    if (watch.byTaskbar && pending && d.why == appclick::Why::Again) {
        ++watch.clicks;
        if (!watch.wantDown && IsWindow(watch.window) && !IsIconic(watch.window) && GetForegroundWindow() != watch.window) {
            TakeForegroundRight();
            ForceForeground(watch.window);
            watch.forced = true;
        }
        a.attention = false;
        return;
    }
    // a start through its pinned button that may still be on its way (Explorer busy): no second start on top of it
    if (d.action == appclick::Action::Launch)
        for (const auto& l : launchChecks)
            if (l.item == a.id && l.maybe) { a.attention = false; return; }
    // the log: the cases the old rules got wrong, the first click again while a window is coming, and
    // which window came up when the app has several (once a minute an app: an unseen window of its
    // own taken for the one to bring up shows there)
    const bool among = usable > 1 && d.action == appclick::Action::Activate && d.why != appclick::Why::Again && now - a.lastChoiceLog > 60;
    const bool againFirst = d.why == appclick::Why::Again && !(pending && watch.clicks > 1);
    if (d.why == appclick::Why::ActiveMinimized || d.skipped || d.overlays || (d.why == appclick::Why::NoWindow && !a.windows.empty()) ||
        againFirst || among) {
        std::wstring which;
        if (among) {
            a.lastChoiceLog = now;
            which = L" (window " + Describe(target) + L", of " + std::to_wstring(usable) + L")";
        }
        Log(L"app click: %ls - %ls%ls%ls%ls%ls", a.name.c_str(), appclick::WhyText(d.why),
            d.skipped ? L" (passed over a window hidden meanwhile)" : L"", d.overlays ? L" (passed over a click-through overlay)" : L"",
            among && d.offScreen ? L" (passed over a window off the screens)" : L"", which.c_str());
    }
    // a restore command of the dock's own for that window still on its way (its program busy; the watch that sent it
    // has ended): no press on top of it - the dock waits for that command (BringUp)
    if (taskbar && target && RestoreUnderWay(target, now)) taskbar = false;
    if (taskbar) {
        taskbarapps::Click c;
        c.enabled = g_settings.taskbarAppButtons;
        c.use = pressUse.use;
        c.action = d.action;
        c.why = d.why;
        c.usable = usable;
        c.windows = (int)a.windows.size();
        c.pinned = a.pinned;
        auto none = noButtonUntil.find(a.id);
        c.noButton = none != noButtonUntil.end() && now < none->second;
        c.slow = now < slowUntil;
        c.launches = !launchUse.off;
        if (d.action == appclick::Action::Activate && target) {
            const HWND fg = GetForegroundWindow();
            DWORD fgPid = 0, targetPid = 0;
            if (fg) GetWindowThreadProcessId(fg, &fgPid);
            GetWindowThreadProcessId(target, &targetPid);
            c.frontIsItsOwn = fg && fg != target && !IsTaskbarWindow(fg) &&
                              (GetAncestor(fg, GA_ROOTOWNER) == target || (targetPid && fgPid == targetPid));
        }
        if (taskbarapps::ByTaskbar(c) && StartPress(a, d, dockIndex, monitor, usable, now)) { a.attention = false; return; }
    }
    switch (d.action) {
    case appclick::Action::Launch:
        Launch(a, dockIndex);
        break;
    case appclick::Action::Minimize:
        if (!adminhelper::Minimize(target, a.name)) PostMessageW(target, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        break;
    case appclick::Action::Activate:
        BringUp(target, appclick::WhyText(d.why), usable);
        break;
    }
    a.attention = false;
}

// The windows the app had at the click, each asked once: the helper may first ask the user whether to install it
// (a message box, admin_helper.cpp Offer), and the dock's messages meanwhile may refresh a.windows.
void AppModel::CloseAll(AppItem& a) {
    const std::vector<HWND> wins = a.windows;
    const std::wstring name = a.name;
    for (HWND h : wins)
        if (!adminhelper::Close(h, name)) PostMessageW(h, WM_CLOSE, 0, 0);
}

// One window, from a preview card's close button or a middle click: as the window's own close button
// (SC_CLOSE - a program that has turned its close button off is left alone, as there). A program that
// asks to save first may bring that question to the front: the dock hands it the right it has from the
// click. Windows of programs that run as administrator go through the helper, as for every other command.
void AppModel::AskToClose(HWND h, const std::wstring& app) {
    if (adminhelper::Close(h, app)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    TakeForegroundRight();
    if (pid) AllowSetForegroundWindow(pid);
    if (!PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0))
        Log(L"preview: could not ask %ls to close a window (error %lu)", app.c_str(), GetLastError());
}
