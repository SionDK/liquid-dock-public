#pragma once
// Windows of programs that run as administrator (admin_protocol.h has the why): the dock asks the
// optional elevated helper to restore, minimize or close them. Setting adminWindows: 0 not asked yet,
// 1 on, 2 off (the dock then tries itself, which Windows refuses for these windows).
#include "common.h"
#include <string>

namespace adminhelper {

// True when the helper takes the action (now, or as soon as it is up); false: the dock does it itself,
// as for every other window. From the UI thread right after a click (the helper may then bring the
// window to the front). The first time, the dock offers to install the helper.
bool Activate(HWND h, const std::wstring& app);
bool Minimize(HWND h, const std::wstring& app);
bool Close(HWND h, const std::wstring& app);

// An ordinary window, h, while a window that runs as administrator is in front: Windows does not let the dock
// take the front from that one, but lets the helper (it runs as administrator too). Only a helper that is on and
// answering: nothing is started, offered or queued here - the dock's own calls go instead. true: the helper took
// the request (whether the window came, the dock's watch sees).
bool ActivateOver(HWND h, const std::wstring& app);

void Tick(double now);     // keeps the helper running while the option is on
// settings: turn on, installing or updating the helper (one UAC prompt) when NeedsInstall - otherwise just on
void Enable(HWND owner);
// Whether turning on the helper's features installs it (a UAC prompt): none in %ProgramFiles%\LiquidDock, or
// not byte for byte the copy next to the dock (adminhelper::HelperNeedsInstall), or the same but its scheduled
// task not registered (SensorClient::HelperTaskRegistered). Reads both files whole and may run schtasks.exe: on a
// click only (Enable, SensorClient::EnablePawnIO), not on a timer.
bool NeedsInstall();

}   // namespace adminhelper
