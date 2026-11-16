#pragma once
#include "common.h"
#include "taskbar_apps_logic.h"
#include <atomic>
#include <memory>
#include <string_view>

// An app's button on Explorer's own taskbar (hidden while the dock runs), pressed through UI Automation so a
// click on the app's dock icon is a click on the taskbar: Windows brings the window up, minimizes it, or starts
// the pinned app, as from its taskbar. Which button is the app's, when a click goes this way and what the dock
// makes of what follows: taskbar_apps_logic.h; AppModel::Click (apps_click.cpp) uses both.
//
// The buttons are XAML, reachable while the taskbar is hidden from its XAML island's windows (tests\
// run_taskbar_probe.bat; taskbar_start.h finds the Start button the same way). Each press looks the button up
// anew - one call brings every app button's id and name (FindAllBuildCache), a few milliseconds - on a thread
// of its own in the process's multithreaded apartment, so the dock never waits on Explorer. Focus is never
// moved to the hidden taskbar (AutoSetFocus off): a focused auto-hide taskbar slides in.
namespace taskbarapps {

struct Job {
    uint32_t id = 0;
    HMONITOR monitor = nullptr;   // the clicked dock's: its monitor's taskbar is looked at first
    std::wstring aumid, exe;      // the window's (else the app's: AppItem), exe a full path
    std::wstring otherAumid;      // the app's, when its window has another (or none of its own): either names its button
    bool running = true;          // a running app the dock counts one window of (else a launch): taskbar_apps_logic.h CountFits
    HWND wake = nullptr;          // posted WM_NULL once the result is in (the dock's loop may be asleep)
    // shared with the dock: whether the press is still wanted (taskbar_apps_logic.h Stage)
    std::shared_ptr<std::atomic<int>> stage = std::make_shared<std::atomic<int>>(kLooking);
};

const wchar_t* OutcomeText(Outcome o);   // Outcome: taskbar_apps_logic.h

struct Result {
    uint32_t job = 0;
    Outcome outcome = Outcome::Failed;
    HRESULT hr = S_OK;
    const wchar_t* how = L"";   // "invoke", "toggle" - or why there was no press
    double lookup = 0;          // s: looking the button up
    double took = 0;            // s: the press call itself (long: Explorer is busy)
    int buttons = 0;            // app buttons seen
    int count = -1;             // the windows its button counts (taskbar_apps_logic.h WindowCount)
    bool slow = false;          // Failed: Explorer too slow to answer (the lookup cut short, a call timed out before any press)
    HWND taskbar = nullptr;     // the taskbar the button was found on (Explorer's thread for it is asked after the press)
    std::wstring button, name;  // the button's id (without "Appid: ") and name
};

// Look the app's button up and press it, on a thread of its own; the result comes through Take (none once the
// dock has abandoned the job: taskbar_apps_logic.h Abandon). false: no thread to be had (the dock does the
// click itself). Any thread.
bool Begin(const Job& job);
// The result of that job, once it is in. Any thread.
bool Take(uint32_t job, Result& out);

// The same, here and now. MTA threads only (the live test).
Result PressNow(const Job& job);

// Every app button on the taskbars, the main one first: its id (without "Appid: ") and name. MTA threads only
// (the live test).
struct Button { std::wstring id, name; HWND taskbar = nullptr; };
std::vector<Button> List(double* took = nullptr);

// The folder a known folder's id stands for ("{7C5A40EF-...}" -> "C:\Program Files (x86)"); "" unknown.
std::wstring KnownFolderPath(std::wstring_view id);

}   // namespace taskbarapps
