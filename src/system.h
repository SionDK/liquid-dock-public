#pragma once
#include "common.h"
#include "battery.h"
#include "native_taskbar.h"   // NativeTaskbar, g_taskbar: the native taskbar hidden while the dock runs
#include "shell_front.h"      // keys, the right to the foreground, the shell's pop-ups, Start shown (shell_front.cpp)
#include <atomic>

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
