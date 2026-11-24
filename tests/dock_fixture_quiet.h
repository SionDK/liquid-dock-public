#pragma once
// Link stubs and shell fakes of the dock tests that only answer and count nothing (dock_render_device_test,
// dock_render_settings_window_test; dock_interaction_test and dock_layout_test have their own, which count or
// trace). Include it after the test's src\ and Windows headers, where the test's other stubs are (it includes two
// src\ headers: keep it at the same place). The shell fakes act where the test's #define list names them.
namespace nativetray { static bool MatrixToggleDesktop() { return true; } }
static bool themeLight = false;
namespace theme {
bool IsLight() { return themeLight; }
bool Toggle() { themeLight = !themeLight; return true; }
}
// Shell, Recycle Bin and audio stubs: the tests never open pages, delete files or switch devices
static HINSTANCE WINAPI MatrixShellExecute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) { return (HINSTANCE)42; }
static int MatrixFileOperation(LPSHFILEOPSTRUCTW) { return 0; }
static HRESULT WINAPI MatrixEmptyRecycleBin(HWND, LPCWSTR, DWORD) { return S_OK; }
#include "../src/audio_devices.h"
namespace audio {
std::vector<Device> Outputs() { return { { L"a", L"Speakers", true }, { L"b", L"Headset", false } }; }
bool SetDefault(const std::wstring&) { return true; }
std::wstring DefaultOutputName() { return L"Speakers"; }   // the audio button's tooltip (since mod.15)
}
// start button stubs: the tests never open Start or press the Windows key
#include "../src/start_menu.h"
namespace startmenu {
bool InFront() { return false; }
void Click(HMONITOR, bool) {}
}
