#pragma once
// A click on an app's dock icon pressing that app's button on Explorer's own (hidden) taskbar, so Windows
// handles it as a click on the taskbar: which button is the app's, when a click goes that way, and what
// the dock makes of what follows (taskbar_apps.h finds and presses the button; AppModel::Click and
// CheckWatch in apps_click.cpp use these). Kept free of Windows headers: tests/taskbar_apps_test.cpp, run by
// tests/run_portable.sh.
//
// The way of a click (a design choice, 2026-10-27): an app with one window, and a pinned app that is not
// running, go through the taskbar's button; an app with several windows stays with the dock's own rules and
// preview (the taskbar's button would show Windows' thumbnails at the hidden taskbar's place, not at the dock).
// Everything the taskbar's way cannot do goes the dock's way, at once: no button found for the app, no button
// counting the windows the dock does, a press that surely did not go through, Explorer too slow to answer - and
// a button that offers no press that works (NoWay): on a 25H2 build an app button offers neither Invoke
// nor Toggle, only the accessibility default action, and that answered S_OK and did nothing - with the taskbar
// hidden or shown, and with Explorer given the right to the front (taskbar_apps.cpp, ReadWays; measured 2026-10-27;
// tests\run_taskbar_apps_diag.bat repeats the measurement).
#include "app_click.h"
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <string>
#include <string_view>

namespace taskbarapps {

// ---- which button is the app's
//
// Windows 11's taskbar names each app button (UI Automation, class Taskbar.TaskListButtonAutomationPeer) by
// the app's id: AutomationId "Appid: Chrome", "Appid: Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"; an app
// without an AppUserModelID by its program's path, a known folder written as that folder's id:
// "Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\Kakao\KakaoTalk\KakaoTalk.exe" (Program Files (x86)).
// Seen with tests\run_taskbar_probe.bat on 25H2, 2026-10-27.
constexpr std::wstring_view kAppIdPrefix = L"Appid: ";
constexpr std::wstring_view kButtonClass = L"Taskbar.TaskListButtonAutomationPeer";

inline std::wstring_view AppIdOf(std::wstring_view automationId) {
    if (automationId.size() <= kAppIdPrefix.size() || automationId.substr(0, kAppIdPrefix.size()) != kAppIdPrefix) return {};
    return automationId.substr(kAppIdPrefix.size());
}

// Without case, a slash taken for a backslash: as Windows compares ids and paths. towlower folds ASCII only
// for sure: the dock hands in text lowered by Windows itself (CharLowerBuffW, taskbar_apps.cpp), as its paths are.
inline bool SameText(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t x = a[i] == L'/' ? L'\\' : a[i], y = b[i] == L'/' ? L'\\' : b[i];
        if (x != y && std::towlower(x) != std::towlower(y)) return false;
    }
    return true;
}

// The folder a known folder's id stands for ("{7C5A40EF-...}" -> "C:\Program Files (x86)"); "" unknown.
using KnownFolder = std::function<std::wstring(std::wstring_view id)>;

// Does this button id (AppIdOf) name the app: its AppUserModelID, or its program's path? The path form is
// read back to a path (the folder's id resolved here) rather than the app's path written in the taskbar's
// form: which known folder Explorer picks for a path is its own business.
inline bool Names(std::wstring_view id, std::wstring_view aumid, std::wstring_view exe, const KnownFolder& knownFolder) {
    if (id.empty()) return false;
    if (id.front() == L'{') {
        // the shell's own AppUserModelID for a program without one is written the same way (shell:AppsFolder):
        // a pin the dock took over from the taskbar or Start may carry it
        if (!aumid.empty() && SameText(id, aumid)) return true;
        const size_t close = id.find(L'}');
        if (close == std::wstring_view::npos || close + 1 >= id.size() || id[close + 1] != L'\\' || exe.empty() || !knownFolder) return false;
        std::wstring path = knownFolder(id.substr(0, close + 1));
        if (path.empty()) return false;
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
        path.append(id.substr(close + 1));
        return SameText(path, exe);
    }
    const bool drive = id.size() > 2 && id[1] == L':' && (id[2] == L'\\' || id[2] == L'/');
    const bool share = id.size() > 2 && id[0] == L'\\' && id[1] == L'\\';
    if (drive || share) return !exe.empty() && SameText(id, exe);
    return !aumid.empty() && SameText(id, aumid);
}

// The windows a button counts, from its name: "<app> - 2개의 실행 중인 창", "<app> - 1 running window";
// a pinned app not running has none ("<app> 고정됨"). -1: no count in the name.
inline int WindowCount(std::wstring_view name) {
    const size_t dash = name.rfind(L" - ");
    if (dash == std::wstring_view::npos) return -1;
    int n = 0, digits = 0;
    for (size_t i = dash + 3; i < name.size() && digits < 4 && name[i] >= L'0' && name[i] <= L'9'; ++i, ++digits) n = n * 10 + (name[i] - L'0');
    return digits ? n : -1;
}

// Is it the button the click means, by the windows it counts (WindowCount)? A click on a running app's one
// window: a button counting one window - one counting more would show Windows' thumbnails at the hidden
// taskbar, and one counting none (its pinned button, while the window has a button of its own under another
// id) would start the app again. A launch: a button counting none - one counting windows the dock does not
// know of would bring those up instead.
inline bool CountFits(bool running, int count) { return running ? count == 1 : count <= 0; }

// ---- when a click goes the taskbar's way
enum class Use : uint8_t {
    Try,     // not seen working yet in this run: tried on clicks the dock can check (a window comes or goes)
    Works,   // a press did what the click wanted: pinned apps not running are started through it too
    Off,     // a press did nothing while the program answered, or the buttons offer no press that works (or the classic
             // taskbar has none): the dock's way for the rest of the run
};

struct Click {
    bool enabled = true;     // settings.ini taskbarAppButtons
    Use use = Use::Try;
    appclick::Action action = appclick::Action::Launch;
    appclick::Why why = appclick::Why::NoWindow;
    int usable = 0;          // windows the dock would show (shown, not click-through overlays)
    int windows = 0;         // all it knows of, hidden ones too
    bool pinned = false;
    bool noButton = false;   // a lookup for this app found no button of its (or not the button meant) in the last kNoButtonAgain
    bool slow = false;       // Explorer was too slow to answer a press in the last kSlowPause
    // the window in front is its program's own, one without a button (a dialog, a tool window): the taskbar
    // takes the app for the active one then and would minimize it, where the dock brings it up - and could
    // not tell whether a press did anything (its program is in front already)
    bool frontIsItsOwn = false;
    bool launches = true;    // launches through the taskbar not given up on this run (LaunchLearning)
};

inline bool ByTaskbar(const Click& c) {
    if (!c.enabled || c.use == Use::Off || c.noButton || c.slow) return false;
    switch (c.action) {
    case appclick::Action::Launch:   // its pinned button starts it - once the taskbar's way has shown it works
        return c.pinned && c.windows == 0 && c.use == Use::Works && c.launches;
    case appclick::Action::Minimize:
        return c.usable == 1 && c.why == appclick::Why::Active;
    case appclick::Action::Activate:   // not Again (a press may be on its way), ActiveMinimized or OffScreen (the dock's own fixes)
        return c.usable == 1 && !c.frontIsItsOwn && (c.why == appclick::Why::Front || c.why == appclick::Why::Minimized);
    }
    return false;
}

// ---- the press on its way: still wanted?
//
// The dock and the worker share a press's stage: Looking until the worker is about to press (Pressing) or
// the dock, done waiting for the lookup, does the click itself (Abandoned) - whichever comes first. So no
// press arrives after the dock went its own way, and the dock never goes its own way once a press may
// arrive. A press that surely did not reach Explorer (its button went away) is looked up again.
enum Stage : int { kLooking = 0, kPressing = 1, kAbandoned = 2 };
constexpr double kLookupWait = 1.0;        // s: no press under way by then (Explorer slow to answer) - the dock does the click itself
constexpr double kPressAnswer = 5.0;       // s: a press under way not answered by then (Explorer hung): given up on, nothing more is sent
// The worker, about to press: true when it may.
inline bool ClaimPress(std::atomic<int>& stage) { int s = kLooking; return stage.compare_exchange_strong(s, kPressing); }
// The dock, done waiting for the lookup: true when no press will come (the dock does the click itself).
inline bool Abandon(std::atomic<int>& stage) { int s = kLooking; return stage.compare_exchange_strong(s, kAbandoned); }
// The worker, its press surely not arrived: looking again.
inline void LookAgain(std::atomic<int>& stage) { int s = kPressing; stage.compare_exchange_strong(s, kLooking); }

// ---- the press's result, and what the dock does next (AppModel::PollPresses)
enum class Outcome : uint8_t {
    Pressed,       // the press went through
    Unsure,        // it failed in a way that may still reach Explorer (a timeout): nothing more is sent for the click
    NoTaskbar,     // Explorer's taskbar is not there
    Classic,       // Windows 10's classic taskbar: no XAML buttons
    NoWay,         // its button offers no press that works (no Invoke, no Toggle): nothing pressed - the app buttons are all
                   // of one kind (Taskbar.TaskListButtonAutomationPeer), so the dock's way for the run
    NoButton,      // no button names the app
    CountDiffers,  // its button counts other windows than the dock does (CountFits)
    Failed,        // the press surely did not go through (refused, the button went away twice, Explorer too slow)
    Abandoned,     // the dock did the click itself meanwhile (Stage): nothing pressed, no result given
};
constexpr double kNoButtonAgain = 60.0;    // s: an app with no button found (or not the button meant) is not looked up again for this long
constexpr double kSlowPause = 30.0;        // s: Explorer too slow to answer (a lookup past kLookupWait, a press timing out): no presses for this long

struct Next {
    bool watch = false;      // pressed for a running app: the dock watches its window (Judge)
    bool launched = false;   // pressed for a launch: the dock's launch bounce, and a look whether the app came
    bool maybe = false;      // ...a press that may or may not have arrived (Unsure): watched as one Explorer is busy with -
                             // never a press that did nothing, nothing of the dock's on top of it (no second start either)
    bool itself = false;     // the dock does the click itself, at once (its decision taken anew)
    bool noButton = false;   // no lookup for this app for kNoButtonAgain
    bool slow = false;       // no presses for kSlowPause
    bool off = false;        // the taskbar's way off for the run
};
// slowLookup: a Failed lookup or press Explorer was too slow for (a call timed out before any press)
inline Next NextAfter(Outcome o, bool launch, bool slowLookup = false) {
    Next n;
    switch (o) {
    case Outcome::Pressed: (launch ? n.launched : n.watch) = true; break;
    case Outcome::Unsure: (launch ? n.launched : n.watch) = n.maybe = n.slow = true; break;   // it may still arrive: watched, nothing on top
    case Outcome::Abandoned: n.slow = n.itself = true; break;
    case Outcome::Classic:
    case Outcome::NoWay: n.off = n.itself = true; break;
    case Outcome::NoButton:
    case Outcome::CountDiffers: n.noButton = n.itself = true; break;
    case Outcome::Failed: n.slow = slowLookup; n.itself = true; break;
    case Outcome::NoTaskbar: n.itself = true; break;
    }
    return n;
}

// ---- after the press: did it do what the click wanted?
constexpr double kPressCheck = 1.2;        // s: a window the taskbar restores, brings up or minimizes has done so by then - unless its program is busy
constexpr double kExplorerBusy = 0.25;     // s: a press call this long - Explorer is busy, what it does may come late

struct Seen {
    double since = 0;            // since the press was answered
    bool wantDown = false;       // the click minimizes it (else it brings it up)
    bool downAtClick = false;    // it was minimized at the click (a bring-up: a restore)
    bool gone = false;           // the window no longer exists
    bool cameUp = false;         // appclick::CameUp
    bool minimized = false;
    bool hidden = false;         // not shown (a program that hides its window to its tray icon when minimized)
    bool elsewhere = false;      // another program came in front since the click (not the taskbar): the user went on
    int answers = -1;            // its program and Explorer's taskbar, asked (WM_NULL) once kPressCheck has passed: -1 not asked yet,
                                 // 0 asked, 1 both answered, 2 its program gave no answer in time (busy), 3 Explorer did not
    bool explorerBusy = false;   // the press call took kExplorerBusy or longer
};

enum class Verdict : uint8_t {
    Wait,    // look again
    Ask,     // ask its program (and Explorer) whether it answers: the dock cannot tell otherwise whether the press arrived
    Done,    // it did what the click wanted
    Toggled, // a bring-up that went down instead: Windows took the press for one on the active app - the press
             // arrived and Windows' doing stands, nothing more
    Behind,  // a restore that left it up behind another window (Windows gave Explorer no front): the press arrived,
             // half done - the dock puts it in front itself (no restore of its own: it is up)
    Stop,    // gone, or the user went on: nothing more, nothing learnt
    Dud,     // its program answers and nothing happened: the press did nothing - the dock does it itself
    GiveUp,  // nothing in appclick::kBusyWait from a busy program or Explorer: a line in the log, nothing more sent
};

inline Verdict Judge(const Seen& s) {
    if (s.gone) return Verdict::Stop;
    if (s.wantDown ? s.minimized || s.hidden : s.cameUp) return Verdict::Done;
    if (!s.wantDown && !s.downAtClick && (s.minimized || s.hidden)) return Verdict::Toggled;
    if (!s.wantDown && s.elsewhere) return Verdict::Stop;
    if (s.since < kPressCheck) return Verdict::Wait;   // (a restore shows before the activation that follows it)
    // restored, and still not in front: the press did come (busy or not, nothing of it is left to wait for)
    if (!s.wantDown && s.downAtClick && !s.minimized && !s.hidden) return Verdict::Behind;
    if (s.explorerBusy || s.answers == 0 || s.answers >= 2) return s.since < appclick::kBusyWait ? Verdict::Wait : Verdict::GiveUp;
    if (s.answers < 0) return Verdict::Ask;
    return Verdict::Dud;
}

// What the run learns from a press it could check. Not working yet: one press that did nothing turns the
// taskbar's way off; once it has worked, only two in a row do (Explorer may have hiccupped once). A press that
// restored the window behind another (Behind) did only half of the click: it shows the press arriving, not the
// taskbar's way working - it counts toward two in a row, and never turns the way off alone (input meanwhile
// may have taken the front from Explorer once). Nothing is learnt of a Behind while Explorer was busy (CheckPress):
// the wait may have cost Explorer its right to the front.
struct Learning {
    Use use = Use::Try;
    int duds = 0;   // in a row
};
// true: use changed (the log says so)
inline bool Learn(Learning& l, Verdict v) {
    const Use before = l.use;
    if (v == Verdict::Done || v == Verdict::Toggled) {   // the press arrived (Toggled: Windows did otherwise, but did)
        l.duds = 0;
        if (l.use == Use::Try) l.use = Use::Works;
    } else if (v == Verdict::Dud) {
        ++l.duds;
        if (l.use == Use::Try || l.duds >= 2) l.use = Use::Off;
    } else if (v == Verdict::Behind) {
        if (++l.duds >= 2) l.use = Use::Off;
    }
    return l.use != before;
}

// ---- launches through the taskbar: did the app start?
// The dock cannot tell a pinned button's press from its start (no window to watch yet): a launch that shows
// no new window of the app in kLaunchCheck is a miss. Two in a row (an app may start to its tray icon once):
// the dock starts pinned apps itself for the rest of the run. Nothing is started again for a miss - the app
// may only be slow, and a second start would be a second copy.
constexpr double kLaunchCheck = 10.0;   // s
struct LaunchLearning {
    bool off = false;
    int misses = 0;   // in a row
};
// came: a new window of the app showed within kLaunchCheck; maybe: a press that may not have arrived (Unsure) -
// no window then is no miss of the button's. true: launches turned off now (the log says so)
inline bool LearnLaunch(LaunchLearning& l, bool came, bool maybe = false) {
    if (l.off) return false;
    if (came) { l.misses = 0; return false; }
    if (maybe) return false;
    l.off = ++l.misses >= 2;
    return l.off;
}

}   // namespace taskbarapps
