#pragma once
// The dock's base (common.h includes it, with spring.h and settings.h): the Windows and C++ headers every part
// uses, the log, strings, files and worker threads (common.cpp). The graphics headers (D3D, D2D, DirectWrite,
// DirectComposition, WIC) are here too, so every file that includes common.h has them.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265358979f;

inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

double NowSeconds();
void Log(const wchar_t* fmt, ...);
// dock.log is started afresh by the first Log of a run of the exe, unless LogAppend came first: the exe's
// other runs (the restore shortcut, --quit, an elevated copy that hands over to the user's) call it so
// they add to the log of the session that just ended, or of the one that runs, instead of wiping it. The
// dock instance calls LogStartSession once it owns the instance mutex: started afresh, even after LogAppend,
// with the lines this run logged before it (at most 16, e.g. why it stays elevated) at the top.
void LogAppend();
void LogStartSession();
std::wstring AppDataDir();          // the "data" folder next to the exe (created): common.cpp
std::wstring ToLower(std::wstring s);
std::string  ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);
std::wstring SystemExe(const wchar_t* name);   // "C:\Windows\System32\<name>", quoted
// The dock's own records (settings.ini, pins.txt, oled.ini): text written to <path>.tmp, put on the disk and
// swapped in, so a crash, a power loss or a full disk leaves the last file whole. False when it was not
// replaced; the cause is in dock.log, said for what ("settings", "pins", "oled"). Flushing waits for the disk a
// moment: at a change the user makes (or a one-time import at the start), never on a timer.
bool SaveFileWhole(const std::wstring& path, const std::string& text, const wchar_t* what);
// A worker thread nobody waits for. false when the system has no thread to give (its commit memory used up):
// std::thread throws then, and out of a window procedure or the main loop that would end the dock - the caller
// does without the worker and says so in dock.log.
template <class F> bool TryDetach(F&& work) {
    try { std::thread(std::forward<F>(work)).detach(); return true; }
    catch (...) { return false; }
}
