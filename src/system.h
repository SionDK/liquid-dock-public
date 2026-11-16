#pragma once
#include "common.h"
#include "battery.h"
#include <atomic>
#include <mutex>

struct SysInfo {
    std::atomic<float> volume{ 0.5f };
    std::atomic<bool>  muted{ false };
    std::atomic<int>   net{ 1 };              // 0 offline, 1 ethernet, 2 wifi
    std::wstring timeText, dateText, timeShort;   // UI thread only
    std::atomic<int>   version{ 0 };          // bumps whenever something visible changed

    void Start();               // the background poller of volume / network / battery (sys_poll.h)
    void Tick(double now);      // clock text (UI thread)
    void AddVolume(float delta);
    void ToggleMute();
    // The battery (battery.h): polled every few seconds by Start's thread and at once on a power change
    // (WM_POWERBROADCAST, the UI thread). Not present on a desktop, and until the first poll.
    void PollBattery();
    void SetBattery(const battery::State& now);   // as the newest read (the dock interaction test sets one)
    battery::State Battery() const;
    bool Publish(float level, bool mute);         // volume and mute as read or reported (any thread); true: changed

private:
    double lastFast = 0;
    // The battery in one word (system.cpp: PackBattery), so a frame never shows half of a change, with the
    // ticket of the read it came from: two threads read it, and a read stored late must not hide a newer one.
    std::atomic<uint64_t> bat{ 0 };
    std::atomic<uint32_t> batReads{ 0 };      // tickets, taken before Windows is asked
    bool StoreBattery(const battery::State& now, uint32_t ticket, battery::State* was = nullptr);
};
extern SysInfo g_sys;

// Hides the native Windows taskbar while the dock runs and restores it afterwards (system_taskbar.cpp).
class NativeTaskbar {
public:
    bool Hide();
    bool Restore();             // false keeps the saved state available for a later retry
    // Re-hide if explorer showed it again. The periodic check runs on its own thread
    // (StartEnforcer) because explorer calls can block for seconds after logon or resume.
    // checkAutoHide: also ask Explorer whether its taskbar still auto-hides (a round trip to it).
    void Enforce(bool onUiThread = true, bool checkAutoHide = true);
    void StartEnforcer();
    void RecheckSoon() { recheck = true; }   // taskbar settings changed: the enforcer asks on its next pass
    bool WithInvisibleTaskbars(const std::function<bool()>& action);
    std::atomic<bool> hidden{ false };
private:
    std::atomic<bool> recheck{ false };
    std::recursive_mutex mu;
    UINT_PTR originalState = 0;
    bool stateSaved = false;
};
extern NativeTaskbar g_taskbar;

// Presses the keys and lets them go (SendInput). true: every event was inserted - which does not mean
// they arrived: Windows drops injected input bound for a program that runs as administrator (UIPI).
bool SendKeyCombo(std::initializer_list<WORD> keys);

// The right to put another window in front (SetForegroundWindow, and to pass it on with
// AllowSetForegroundWindow). The dock's windows never take activation (WS_EX_NOACTIVATE), and a click on
// such a window does not always give the dock that right: SetForegroundWindow then only flashes the
// window (2026-10-27: nine clicks on two app icons, the windows stayed behind until
// Alt+Tab). Input this process sends itself gives it the right: an unassigned key (0xE8) that no program
// acts on - RetroBar does the same for its taskbar. Probed first (AllowSetForegroundWindow on this very
// process fails without the right), so the key goes out only when the right is missing.
enum class ForegroundRight { Had, Taken, Refused, NotInserted };
ForegroundRight TakeForegroundRight();
const wchar_t* ForegroundRightText(ForegroundRight r);

// The shell's own pop-ups (Start, Search, quick settings, the notification centre), by the program of the
// window: the dock comes up with them (Dock::ShellPopupOpen), and a click on an app icon closes the one in
// front before it brings the app up (AppModel::BringUp), as a click on the taskbar does.
bool IsShellPopupWindow(HWND w);
