#pragma once
// Shared by the parts of the dock's process: main.cpp (entry point, main loop, window procedure),
// main_instance.cpp (a start as administrator handed to the user, the crash handler, the watchdog copy) and
// main_autostart.cpp (starting with Windows: the Run value and the logon task).
#include "common.h"

static constexpr const wchar_t* kInstanceMutex = L"LiquidDock.SingleInstance";

// main_autostart.cpp
void SetAutostart(bool on);

// main_instance.cpp
extern HANDLE g_watchdogProcess;
bool WatchdogReady();
bool RestartUnelevated(const std::wstring& args);
LONG WINAPI CrashHandler(EXCEPTION_POINTERS*);
int RestoreTaskbarWhenIdle(DWORD timeoutMs);
int RunWatchdog(DWORD pid, const wchar_t* readyName);
bool StartWatchdog();
