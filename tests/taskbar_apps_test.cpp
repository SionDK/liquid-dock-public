// Which taskbar button is a dock app's, when a click on its icon presses it, and what the dock makes of
// what follows (src/taskbar_apps_logic.h). No Windows headers: builds with any C++20 compiler.
//   cl /std:c++20 /EHsc /utf-8 tests\taskbar_apps_test.cpp
//   g++ -std=c++20 tests/taskbar_apps_test.cpp -o taskbar_apps_test
#include "../src/taskbar_apps_logic.h"
#include "portable_check.h"
#include <string>
#include <vector>

using namespace taskbarapps;
using appclick::Action;
using appclick::Why;

namespace {
// The known folders the probe saw (25H2, 2026-10-27), as SHGetKnownFolderPath gives them.
std::wstring Folder(std::wstring_view id) {
    if (SameText(id, L"{7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}")) return L"C:\\Program Files (x86)";
    if (SameText(id, L"{6D809377-6AF0-444B-8957-A3773F02200E}")) return L"C:\\Program Files";
    if (SameText(id, L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}")) return L"C:\\WINDOWS\\system32";
    if (SameText(id, L"{374DE290-123F-4565-9164-39C4925E467B}")) return L"C:\\Users\\User\\Downloads\\";   // a trailing backslash, for once
    return L"";
}
bool N(std::wstring_view automationId, std::wstring_view aumid, std::wstring_view exe) {
    return Names(AppIdOf(automationId), aumid, exe, Folder);
}
Click C(Action a, Why w, int usable, int windows = -1, bool pinned = false, Use use = Use::Try) {
    Click c;
    c.action = a; c.why = w; c.usable = usable; c.windows = windows < 0 ? usable : windows; c.pinned = pinned; c.use = use;
    return c;
}
Seen S(double since, bool wantDown = false) { Seen s; s.since = since; s.wantDown = wantDown; return s; }
}

int main() {
    // ---- the button's id
    Check(AppIdOf(L"Appid: Chrome") == L"Chrome", "an app button's id: what follows \"Appid: \"");
    Check(AppIdOf(L"StartButton").empty() && AppIdOf(L"Appid: ").empty() && AppIdOf(L"appid: Chrome").empty() && AppIdOf(L"").empty(),
          "the Start button, an empty id or another spelling: no app button");

    // ---- by AppUserModelID (the ids the probe saw)
    Check(N(L"Appid: Chrome", L"Chrome", L"c:\\program files\\google\\chrome\\application\\chrome.exe"), "Chrome: its AppUserModelID");
    Check(N(L"Appid: Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", L"microsoft.windowsterminal_8wekyb3d8bbwe!app", L""), "a store app, ids compared without case");
    Check(N(L"Appid: Claude_pzs8sxrjxfjjc!Claude", L"Claude_pzs8sxrjxfjjc!Claude", L"c:\\program files\\windowsapps\\claude\\claude.exe"), "Claude");
    Check(!N(L"Appid: Chrome", L"", L"c:\\program files\\google\\chrome\\application\\chrome.exe"), "an id the app does not have is never taken for its path");
    Check(!N(L"Appid: Chrome", L"Chrome.Profile2", L""), "another of the program's ids (another profile) is another app");
    Check(!N(L"Appid: Chrom", L"Chrome", L""), "...and a prefix is no match");

    // ---- by the program's path, a known folder written as its id
    const std::wstring kakao = L"c:\\program files (x86)\\kakao\\kakaotalk\\kakaotalk.exe";
    Check(N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"", kakao), "KakaoTalk: Program Files (x86)'s id and the rest of its path");
    Check(N(L"Appid: {7c5a40ef-a0fb-4bfc-874a-c0f2e0b9fa8e}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"", kakao), "...the folder's id in any case");
    Check(N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"some.other.id", kakao), "...whatever id the dock found for it");
    Check(N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Steam\\steamapps\\common\\Lossless Scaling\\LosslessScaling.exe", L"",
            L"c:\\program files (x86)\\steam\\steamapps\\common\\lossless scaling\\losslessscaling.exe"), "Lossless Scaling, spaces in its path");
    Check(N(L"Appid: {1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\notepad.exe", L"", L"c:\\windows\\system32\\notepad.exe"), "System32");
    Check(N(L"Appid: {374DE290-123F-4565-9164-39C4925E467B}\\liquid-dock\\build\\tests\\x.exe", L"", L"c:\\users\\user\\downloads\\liquid-dock\\build\\tests\\x.exe"),
          "a folder path given with a trailing backslash");
    Check(!N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"", L"c:\\program files\\kakao\\kakaotalk\\kakaotalk.exe"),
          "the same program in another folder is another app");
    Check(!N(L"Appid: {00000000-0000-0000-0000-000000000000}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"", kakao), "a folder id the system does not know: no match");
    Check(!N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"", L""), "no path known for the app: no match by path");
    Check(N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"{7c5a40ef-a0fb-4bfc-874a-c0f2e0b9fa8e}\\kakao\\kakaotalk\\kakaotalk.exe", L""),
          "...but the shell's own id for it (a pin taken over from the taskbar or Start) names it, path or not");
    Check(N(L"Appid: {00000000-0000-0000-0000-000000000000}\\Tool\\tool.exe", L"{00000000-0000-0000-0000-000000000000}\\tool\\tool.exe", L""),
          "...even with a folder id the system does not know");
    Check(!N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}", L"", kakao) && !N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}Kakao", L"", kakao) &&
          !N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E\\Kakao", L"", kakao), "malformed path ids match nothing");
    Check(!Names(L"{7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"", kakao, nullptr), "no folder resolver: no match by path");

    // ---- by a full path
    Check(N(L"Appid: D:\\Games\\Tool\\tool.exe", L"", L"d:\\games\\tool\\tool.exe"), "a path outside every known folder");
    Check(N(L"Appid: D:/Games/Tool/tool.exe", L"", L"d:\\games\\tool\\tool.exe"), "...slashes either way");
    Check(N(L"Appid: \\\\server\\share\\tool.exe", L"", L"\\\\server\\share\\tool.exe"), "a network path");
    Check(!N(L"Appid: D:\\Games\\Tool\\tool.exe", L"D:\\Games\\Tool\\tool.exe", L""), "a path is compared with the program's path, not its id");
    {   // known folders moved by the user: to a drive's root, written with slashes
        const KnownFolder moved = [](std::wstring_view id) -> std::wstring {
            if (SameText(id, L"{374DE290-123F-4565-9164-39C4925E467B}")) return L"D:\\";
            if (SameText(id, L"{F1B32785-6FBA-4FCF-9D55-7B8E7F157091}")) return L"E:/Users/x/AppData/Local//";
            return L"";
        };
        Check(Names(L"{374DE290-123F-4565-9164-39C4925E467B}\\tool.exe", L"", L"d:\\tool.exe", moved), "Downloads moved to a drive's root (D:\\): its programs still named");
        Check(Names(L"{F1B32785-6FBA-4FCF-9D55-7B8E7F157091}\\Discord\\Update.exe", L"", L"e:\\users\\x\\appdata\\local\\discord\\update.exe", moved),
              "a folder path with slashes and trailing ones: compared as Windows does");
        Check(!Names(L"{374DE290-123F-4565-9164-39C4925E467B}\\", L"", L"d:\\tool.exe", moved), "a path id naming the folder alone: none of its programs");
    }
    Check(!N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"", L"c:\\program files (x86)\\kakao\\kakaotalk\\kakaotalk.exe.bak") &&
          !N(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"", L"c:\\program files (x86)\\kakao\\kakaotalk"),
          "a path is the whole path: no prefix and no longer path matches");

    // ---- the windows a button counts
    Check(WindowCount(L"터미널 - 2개의 실행 중인 창") == 2 && WindowCount(L"Chrome - 1개의 실행 중인 창 고정됨") == 1, "Korean: 2 windows; 1 window, pinned");
    Check(WindowCount(L"Terminal - 2 running windows") == 2 && WindowCount(L"Chrome - 12 running windows") == 12, "English");
    Check(WindowCount(L"카카오톡 고정됨") == -1 && WindowCount(L"") == -1, "a pinned app not running: no count");
    Check(WindowCount(L"Visual Studio Code - Insiders 고정됨") == -1, "a dash in the app's name is no count");
    Check(WindowCount(L"Visual Studio Code - Insiders - 3 running windows") == 3, "...the last dash is");
    Check(WindowCount(L"Chrome - 1 running window") == 1 && WindowCount(L"터미널 - 10개의 실행 중인 창 고정됨") == 10, "one window in English; ten, pinned, in Korean");
    Check(WindowCount(L"Chrome - 123456 running windows") == 1234 && !CountFits(true, WindowCount(L"Chrome - 123456 running windows")),
          "at most four digits are read: a huge count is still never taken for one window");
    Check(WindowCount(L"Chrome - ") == -1 && WindowCount(L"Chrome -1 running window") == -1 && WindowCount(L"Chrome - running windows") == -1,
          "a dash with no count after it (or not set apart by spaces): no count");
    Check(WindowCount(L"Chrome – 1 running window") == -1 && !CountFits(true, WindowCount(L"Chrome – 1 running window")),
          "a language that writes it otherwise (an en dash): no count read - the dock's way for a running app, the safe side");
    Check(WindowCount(L"Chrome - 1 running window - Profile 2") == -1 && !CountFits(true, WindowCount(L"Chrome - 1 running window - Profile 2")),
          "a count not after the last dash: not read, never taken for one window");
    Check(!CountFits(false, WindowCount(L"Python 3.12 - 64-bit 고정됨")),
          "a pinned app whose name has a dash and digits: read as a count - a launch goes the dock's way (the safe side), never a press of a running app's button");

    // ---- the button meant, by the windows it counts
    Check(CountFits(true, 1), "a running app's one window: its button counting one window");
    Check(!CountFits(true, 2) && !CountFits(true, 12), "...not one counting more (Windows' thumbnails would come up at the hidden taskbar)");
    Check(!CountFits(true, -1) && !CountFits(true, 0), "...nor its pinned button counting none (the window's button is another: a press would start the app again)");
    Check(CountFits(false, -1) && CountFits(false, 0), "a launch: its pinned button, counting no window");
    Check(!CountFits(false, 1) && !CountFits(false, 3), "...not a button counting windows the dock does not know of (a press would bring those up)");
    Check(CountFits(true, WindowCount(L"Chrome - 1개의 실행 중인 창 고정됨")) && CountFits(false, WindowCount(L"카카오톡 고정됨")),
          "the names the probe saw: one window, pinned; pinned, not running");

    // ---- when a click goes the taskbar's way
    Check(ByTaskbar(C(Action::Activate, Why::Front, 1)), "one window behind others: the taskbar brings it up");
    Check(ByTaskbar(C(Action::Activate, Why::Minimized, 1)), "one window minimized: the taskbar restores it");
    Check(ByTaskbar(C(Action::Minimize, Why::Active, 1)), "one window, the active one: the taskbar minimizes it");
    Check(ByTaskbar(C(Action::Activate, Why::Front, 1, 3)), "one window shown, others hidden: still one window for the taskbar");
    Check(!ByTaskbar(C(Action::Activate, Why::Cycle, 2)) && !ByTaskbar(C(Action::Activate, Why::Front, 2)) && !ByTaskbar(C(Action::Activate, Why::Minimized, 3)),
          "several windows: the dock's own rules and preview");
    Check(!ByTaskbar(C(Action::Minimize, Why::Active, 2)), "a minimize among several windows (app_click.h never decides one): the dock's way");
    Check(!ByTaskbar(C(Action::Activate, Why::Again, 1)), "clicked again while its window is coming: never a second press");
    Check(!ByTaskbar(C(Action::Activate, Why::ActiveMinimized, 1)) && !ByTaskbar(C(Action::Activate, Why::OffScreen, 1)),
          "the cases the dock's rules fix (minimized but active, off the screens): the dock's way");
    Check(!ByTaskbar(C(Action::Launch, Why::NoWindow, 0, 0, true, Use::Try)), "a pinned app not running, the taskbar's way not seen working yet: the dock starts it");
    Check(ByTaskbar(C(Action::Launch, Why::NoWindow, 0, 0, true, Use::Works)), "...seen working: its pinned button starts it");
    Check(!ByTaskbar(C(Action::Launch, Why::NoWindow, 0, 0, false, Use::Works)), "an app not pinned (the menu's \"open\"): the dock starts it");
    Check(!ByTaskbar(C(Action::Launch, Why::NoWindow, 0, 2, true, Use::Works)), "only hidden windows (a program in its tray icon): the dock starts it again");
    {
        Click off = C(Action::Activate, Why::Front, 1, -1, false, Use::Off), none = C(Action::Activate, Why::Front, 1), disabled = none, slow = none;
        none.noButton = true; disabled.enabled = false; slow.slow = true;
        Check(!ByTaskbar(off) && !ByTaskbar(none) && !ByTaskbar(disabled) && !ByTaskbar(slow),
              "turned off, no button found lately, off in the settings, or Explorer slow to answer lately: the dock's way");
        Click launch = C(Action::Launch, Why::NoWindow, 0, 0, true, Use::Works), minimize = C(Action::Minimize, Why::Active, 1);
        launch.slow = minimize.slow = true;
        Check(!ByTaskbar(launch) && !ByTaskbar(minimize), "...for every kind of click");
        Check(!ByTaskbar(C(Action::Minimize, Why::Active, 1, -1, false, Use::Off)), "turned off: a minimize goes the dock's way too");
    }
    {
        Click front = C(Action::Activate, Why::Front, 1), restore = C(Action::Activate, Why::Minimized, 1), down = C(Action::Minimize, Why::Active, 1);
        front.frontIsItsOwn = restore.frontIsItsOwn = down.frontIsItsOwn = true;
        Check(!ByTaskbar(front) && !ByTaskbar(restore),
              "its own dialog in front (a window without a button): the dock brings it up (the taskbar would minimize it, unseen by the dock)");
        Check(ByTaskbar(down), "...a minimize of the active window is not affected");
        Click launch = C(Action::Launch, Why::NoWindow, 0, 0, true, Use::Works);
        launch.launches = false;
        Check(!ByTaskbar(launch), "launches through the taskbar given up on: the dock starts pinned apps itself");
        launch.launches = true;
        Check(ByTaskbar(launch), "...else they go through it");
        Click disabled = launch, none = launch, overlayOnly = launch;
        disabled.enabled = false; none.noButton = true; overlayOnly.windows = 1;
        Check(!ByTaskbar(disabled) && !ByTaskbar(none) && !ByTaskbar(overlayOnly),
              "a launch: off in the settings, no button found lately, or a window of its left (an overlay): the dock starts it");
        Click hiddenToo = C(Action::Activate, Why::Minimized, 1, 2, true, Use::Works);
        Check(ByTaskbar(hiddenToo), "one window minimized, another hidden (to its tray), seen working: the taskbar restores it");
    }

    // ---- the press on its way: the dock and the worker agree whether it is still wanted
    {
        std::atomic<int> st{ kLooking };
        Check(ClaimPress(st) && st == kPressing, "the worker found the button first: it presses");
        Check(!Abandon(st) && st == kPressing, "...and the dock, done waiting, no longer goes its own way (the press may arrive)");
        LookAgain(st);
        Check(st == kLooking, "a press that surely did not arrive (its button went away): looking again");
        Check(Abandon(st) && st == kAbandoned, "...and now the dock, done waiting, may go its own way");
        Check(!ClaimPress(st) && st == kAbandoned, "...after which the worker presses nothing");
        LookAgain(st);
        Check(st == kAbandoned, "...not even after a press that went away");
        std::atomic<int> late{ kLooking };
        Check(Abandon(late) && !ClaimPress(late), "the dock done waiting first: no press at all");
        Check(!Abandon(late), "...given up once");
        Check(kLookupWait < kPressAnswer, "the dock stops waiting for a lookup before it gives up on a press stuck in Explorer");
    }

    // ---- the press's result: what the dock does next
    {
        const Outcome all[] = { Outcome::Pressed, Outcome::Unsure, Outcome::NoTaskbar, Outcome::Classic, Outcome::NoWay, Outcome::NoButton,
                                Outcome::CountDiffers, Outcome::Failed, Outcome::Abandoned };
        bool once = true, pressedOnly = true;
        for (Outcome o : all)
            for (int launch = 0; launch < 2; ++launch)
                for (int slow = 0; slow < 2; ++slow) {
                    const Next n = NextAfter(o, launch != 0, slow != 0);
                    once &= (int)n.watch + (int)n.launched + (int)n.itself <= 1;
                    pressedOnly &= (n.watch || n.launched) == (o == Outcome::Pressed || o == Outcome::Unsure);
                    pressedOnly &= n.maybe == (o == Outcome::Unsure);
                }
        Check(once, "every result: one of watching the window, the launch bounce, the dock doing it itself - never two");
        Check(pressedOnly, "...watching or the launch bounce only after a press that went through, or may have (a timeout: maybe)");
        Next run = NextAfter(Outcome::Pressed, false), start = NextAfter(Outcome::Pressed, true);
        Check(run.watch && !run.launched && start.launched && !start.watch && !run.itself && !start.itself,
              "pressed: a running app's window is watched, a launch bounces");
        Next unsure = NextAfter(Outcome::Unsure, false), unsureStart = NextAfter(Outcome::Unsure, true);
        Check(unsure.watch && unsure.maybe && !unsure.itself && unsure.slow,
              "unsure whether the press arrived: its window watched as one Explorer is busy with (nothing of the dock's on top), no presses for a while");
        Check(unsureStart.launched && unsureStart.maybe && !unsureStart.itself, "...a start likewise: looked for, no second start on top of it");
        Next gaveUp = NextAfter(Outcome::Abandoned, false);
        Check(gaveUp.itself && gaveUp.slow && !gaveUp.off, "Explorer too slow to find it: the dock does it, no presses for a while, not off for the run");
        Check(NextAfter(Outcome::Classic, false).off && NextAfter(Outcome::Classic, false).itself, "the classic taskbar: off for the run, the dock does it");
        {
            bool offItself = true, nothingElse = true;
            for (int launch = 0; launch < 2; ++launch)
                for (int slow = 0; slow < 2; ++slow) {
                    const Next n = NextAfter(Outcome::NoWay, launch != 0, slow != 0);
                    offItself &= n.off && n.itself;
                    nothingElse &= !n.watch && !n.launched && !n.maybe && !n.noButton && !n.slow;
                }
            Check(offItself, "a button with no press that works (a 25H2 build: only the accessibility default action): the dock does it, off for the run");
            Check(nothingElse, "...nothing pressed: nothing watched, no launch looked for, no app left unlooked-for, no pause for a slow Explorer");
            Learning l;
            if (NextAfter(Outcome::NoWay, false).off) l.use = Use::Off;   // as AppModel::PollPresses
            Check(ByTaskbar(C(Action::Activate, Why::Front, 1)) && !ByTaskbar(C(Action::Activate, Why::Front, 1, -1, false, l.use)),
                  "...and the next click goes the dock's way at once, without a lookup");
        }
        Check(NextAfter(Outcome::NoButton, false).noButton && NextAfter(Outcome::CountDiffers, true).noButton &&
              NextAfter(Outcome::NoButton, false).itself && !NextAfter(Outcome::NoButton, false).slow,
              "no button, or not the one meant: the dock does it, and does not look for that app for a while");
        Check(NextAfter(Outcome::Failed, false).itself && !NextAfter(Outcome::Failed, false).slow && NextAfter(Outcome::Failed, false, true).slow,
              "a failed press: the dock does it; a lookup Explorer was too slow for: no presses for a while too");
        Check(NextAfter(Outcome::NoTaskbar, false).itself && !NextAfter(Outcome::NoTaskbar, false).off, "no taskbar (Explorer restarting): the dock does it, not off");
    }

    // ---- after the press
    Check(Judge(S(0.1)) == Verdict::Wait, "a moment after the press: look again");
    { Seen s = S(0.3); s.cameUp = true; Check(Judge(s) == Verdict::Done, "it came up: done"); }
    { Seen s = S(0.3, true); s.minimized = true; Check(Judge(s) == Verdict::Done, "it went down: done"); }
    { Seen s = S(0.3, true); s.hidden = true; Check(Judge(s) == Verdict::Done, "it went to its tray icon (hidden) as it went down: done, no press that did nothing"); }
    { Seen s = S(0.3, true); s.cameUp = true; Check(Judge(s) == Verdict::Wait, "a minimize is not done by the window being up"); }
    { Seen s = S(0.3); s.minimized = true; s.downAtClick = true; Check(Judge(s) == Verdict::Wait, "...nor a restore by its being still down"); }
    { Seen s = S(0.3); s.minimized = true; Check(Judge(s) == Verdict::Toggled,
          "a bring-up of a window that was up, and it went down: Windows took the press for a minimize - it arrived, and stands"); }
    { Seen s = S(0.3); s.hidden = true; Check(Judge(s) == Verdict::Toggled, "...down to its tray icon too"); }
    { Seen s = S(2.0); s.minimized = true; s.answers = 1; Check(Judge(s) == Verdict::Toggled, "...never a press that did nothing (the dock would bring it back up)"); }
    { Seen s = S(0.3); s.minimized = true; s.elsewhere = true; Check(Judge(s) == Verdict::Toggled, "...whatever came in front after it"); }
    { Seen s = S(2.0); s.minimized = true; s.downAtClick = true; s.answers = 1; Check(Judge(s) == Verdict::Dud, "a restore that left it down while its program answers: did nothing"); }
    { Seen s = S(0.3); s.gone = true; Check(Judge(s) == Verdict::Stop, "the window is gone: stop"); }
    { Seen s = S(0.3); s.elsewhere = true; Check(Judge(s) == Verdict::Stop, "the user went on to another program: stop"); }
    { Seen s = S(2.0, true); s.elsewhere = true; Check(Judge(s) == Verdict::Ask, "a minimize brings another window in front: no reason to stop"); }
    Check(Judge(S(kPressCheck)) == Verdict::Ask, "nothing after kPressCheck: ask its program whether it answers");
    { Seen s = S(kPressCheck); s.answers = 0; Check(Judge(s) == Verdict::Wait, "asked, no answer yet: wait"); }
    { Seen s = S(kPressCheck); s.answers = 2; Check(Judge(s) == Verdict::Wait, "its program is busy: the press may still be on its way"); }
    { Seen s = S(appclick::kBusyWait); s.answers = 2; Check(Judge(s) == Verdict::GiveUp, "...until kBusyWait: give up, sending nothing more"); }
    { Seen s = S(kPressCheck); s.answers = 3; Check(Judge(s) == Verdict::Wait, "Explorer gave no answer in time: the press may still be on its way"); }
    { Seen s = S(appclick::kBusyWait); s.answers = 3; Check(Judge(s) == Verdict::GiveUp, "...until kBusyWait"); }
    { Seen s = S(5.0); s.explorerBusy = true; s.answers = 1; Check(Judge(s) == Verdict::Wait,
          "a press that may not have arrived (watched as Explorer busy): never taken for one that did nothing"); }
    { Seen s = S(kPressCheck); s.answers = 1; Check(Judge(s) == Verdict::Dud, "its program answers and nothing happened: the press did nothing"); }
    { Seen s = S(kPressCheck); s.answers = 1; s.explorerBusy = true; Check(Judge(s) == Verdict::Wait, "Explorer was busy with the press: wait"); }
    { Seen s = S(appclick::kBusyWait); s.answers = 1; s.explorerBusy = true; Check(Judge(s) == Verdict::GiveUp, "...until kBusyWait"); }
    { Seen s = S(kPressCheck + 1); s.answers = 1; s.cameUp = true; Check(Judge(s) == Verdict::Done, "late, but it came: done"); }
    // the thresholds, just before and at them
    Check(Judge(S(kPressCheck - 0.01)) == Verdict::Wait, "just before kPressCheck: still looking, nothing asked");
    { Seen s = S(appclick::kBusyWait - 0.01); s.answers = 2; Check(Judge(s) == Verdict::Wait, "a busy program just before kBusyWait: still waited for"); }
    { Seen s = S(appclick::kBusyWait - 0.01); s.explorerBusy = true; Check(Judge(s) == Verdict::Wait, "...a busy Explorer likewise"); }
    { Seen s = S(0.3); s.gone = true; s.cameUp = true; s.minimized = true; Check(Judge(s) == Verdict::Stop, "a window gone: stop, whatever else was seen"); }
    { Seen s = S(kPressCheck, true); s.elsewhere = true; s.answers = 1; Check(Judge(s) == Verdict::Dud,
          "a minimize that left it up while its program answers: did nothing, even with another window in front"); }
    { Seen s = S(0.3); s.downAtClick = true; s.hidden = true; Check(Judge(s) == Verdict::Wait,
          "a restore of a window that went hidden (to its tray icon): not taken for Windows minimizing it"); }
    {   // restored at once, the front not yet taken (the activation follows the restore)
        Seen s = S(0.3); s.downAtClick = true;
        Check(Judge(s) == Verdict::Wait, "a moment after a restore, up but not in front yet: look again");
    }
    {   // Windows restored it but did not give it the front (it refused Explorer the activation): the press arrived
        Seen s = S(kPressCheck); s.downAtClick = true; s.answers = 1;   // no longer minimized, shown, another window in front
        const Verdict v = Judge(s);
        Learning l;
        Learn(l, v);
        Check(v != Verdict::Dud && l.use != Use::Off,
              "a minimized window the press restored, left behind another window: the press arrived - never one that did nothing, nor the taskbar's way off for it");
        Check(v == Verdict::Behind, "...the dock puts it in front itself (Behind)");
    }
    {   // ...a press Explorer was busy with (or one that may not have arrived: maybe), restored behind another window
        Seen s = S(appclick::kBusyWait); s.downAtClick = true; s.explorerBusy = true;
        const Verdict v = Judge(s);
        Check(v != Verdict::GiveUp && v != Verdict::Dud && v != Verdict::Wait,
              "...a press Explorer was busy with likewise: restored behind, it arrived - not given up on after kBusyWait with the window left behind");
    }
    { Seen s = S(kPressCheck); s.downAtClick = true; Check(Judge(s) == Verdict::Behind, "...nothing asked of its program first (the restore shows the press came)"); }
    { Seen s = S(kPressCheck); s.downAtClick = true; s.answers = 2; Check(Judge(s) == Verdict::Behind, "...nor waited for while its program is busy (it is up: no command of its)"); }
    { Seen s = S(kPressCheck - 0.01); s.downAtClick = true; s.explorerBusy = true;
      Check(Judge(s) == Verdict::Wait, "...but before kPressCheck it is looked at again (the activation may follow)"); }
    { Seen s = S(kPressCheck); s.downAtClick = true; s.cameUp = true; Check(Judge(s) == Verdict::Done, "restored and in front: done, not behind"); }
    { Seen s = S(kPressCheck); s.downAtClick = true; s.elsewhere = true; Check(Judge(s) == Verdict::Stop, "restored behind a program the user went on to: stop, nothing put in front"); }
    { Seen s = S(kPressCheck, true); s.downAtClick = true; s.answers = 1; Check(Judge(s) != Verdict::Behind, "a minimize is never behind"); }
    {   // what a restored window behind another is not taken for
        Seen up = S(kPressCheck); up.answers = 1;   // a bring-up of a window that was up: still behind, nothing happened
        Check(Judge(up) == Verdict::Dud, "a window up at the click, still behind after kPressCheck while its program answers: the press did nothing");
        Seen down = S(kPressCheck); down.downAtClick = true; down.minimized = true; down.answers = 1;
        Check(Judge(down) == Verdict::Dud, "...a restore that left it minimized likewise");
        Seen tray = S(kPressCheck); tray.downAtClick = true; tray.hidden = true; tray.answers = 1;
        Check(Judge(tray) == Verdict::Dud, "...and one hidden (to its tray icon), never restored: did nothing");
    }

    // ---- what the run learns
    {
        Learning l;
        Check(!Learn(l, Verdict::Wait) && l.use == Use::Try, "nothing judged: nothing learnt");
        Check(Learn(l, Verdict::Done) && l.use == Use::Works, "a press that did what it should: the taskbar's way works");
        Check(!Learn(l, Verdict::Dud) && l.use == Use::Works, "once working, one press that did nothing does not turn it off");
        Check(!Learn(l, Verdict::Done) && l.use == Use::Works && l.duds == 0, "...a good one in between starts the count again");
        Learn(l, Verdict::Dud);
        Check(Learn(l, Verdict::Dud) && l.use == Use::Off, "two in a row do");
        Check(!Learn(l, Verdict::Done) && l.use == Use::Off, "off stays off for the run");
        {
            Learning off;
            off.use = Use::Off;
            Check(!Learn(off, Verdict::Toggled) && off.use == Use::Off && !Learn(off, Verdict::Dud) && off.use == Use::Off,
                  "...whatever a press still watched turns out to be (off from NoWay, the classic taskbar, presses that did nothing)");
        }
        Learning fresh;
        Check(Learn(fresh, Verdict::Dud) && fresh.use == Use::Off, "not seen working yet: one press that did nothing turns it off");
        Learning busy;
        Check(!Learn(busy, Verdict::GiveUp) && !Learn(busy, Verdict::Stop) && busy.use == Use::Try, "a busy program or a user going on teaches nothing");
        Learning toggled;
        Check(Learn(toggled, Verdict::Toggled) && toggled.use == Use::Works, "a press Windows took otherwise still arrived: the taskbar's way works");
        Learning back;
        Learn(back, Verdict::Done);
        Learn(back, Verdict::Dud);
        Check(!Learn(back, Verdict::Toggled) && back.duds == 0 && back.use == Use::Works, "...and starts the count of presses that did nothing again");
        Learning gap;
        Learn(gap, Verdict::Done);
        Learn(gap, Verdict::Dud);
        Learn(gap, Verdict::GiveUp);
        Learn(gap, Verdict::Stop);
        Check(Learn(gap, Verdict::Dud) && gap.use == Use::Off, "a busy program or a user going on between two presses that did nothing: still two in a row");
        // a restore left behind another window: the press arrived, the click only half done
        Learning behind;
        Check(!Learn(behind, Verdict::Behind) && behind.use == Use::Try && behind.duds == 1,
              "restored behind, not seen working yet: no proof the taskbar's way works (and none that it does nothing)");
        Check(Learn(behind, Verdict::Behind) && behind.use == Use::Off, "...twice in a row: the dock's way (every click a moment late otherwise)");
        Learning worked;
        Learn(worked, Verdict::Done);
        Check(!Learn(worked, Verdict::Behind) && worked.use == Use::Works && worked.duds == 1, "once working, one restore left behind keeps it");
        Check(Learn(worked, Verdict::Dud) && worked.use == Use::Off, "...followed by a press that did nothing: two in a row");
        Learning mixed;
        Learn(mixed, Verdict::Behind);
        Check(Learn(mixed, Verdict::Done) && mixed.use == Use::Works && mixed.duds == 0, "a restore behind, then one that came: the taskbar's way works, the count starts again");
    }

    // ---- launches through the taskbar
    {
        LaunchLearning l;
        Check(!LearnLaunch(l, false) && !l.off, "a launch with no window in kLaunchCheck: one miss (an app may start to its tray icon)");
        Check(LearnLaunch(l, false) && l.off, "two in a row: the dock starts pinned apps itself");
        Check(!LearnLaunch(l, true) && l.off, "...for the rest of the run");
        LaunchLearning m;
        LearnLaunch(m, false);
        LearnLaunch(m, true);
        Check(!LearnLaunch(m, false) && !m.off, "a launch that came in between starts the count again");
        Check(kLaunchCheck >= 5.0, "a slow app is given a while to show its window");
        LaunchLearning q;
        Check(!LearnLaunch(q, false, true) && !LearnLaunch(q, false, true) && !q.off && q.misses == 0,
              "no window after a press that may not have arrived: no miss of the button's");
        LearnLaunch(q, false);
        Check(!LearnLaunch(q, false, true) && q.misses == 1, "...nor does it end a count of misses");
    }
    return Summary("taskbar_apps_test");
}
