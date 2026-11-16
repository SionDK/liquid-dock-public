#pragma once
// What the parts of the app model share (apps.cpp, apps_pins.cpp, apps_click.cpp, apps_press.cpp); each is
// defined once, in the file named.
#include "apps.h"
#include <shobjidl.h>   // IShellItem
extern Settings g_settings;
extern HWND g_mainWnd;   // woken when a taskbar button's press has its result (the loop may be asleep)
// apps.cpp
std::wstring ProcessExe(DWORD pid);
bool IsCloaked(HWND h);
bool IsTaskbarWindow(HWND h);
std::wstring FileStem(const std::wstring& path);
bool EndsWithI(const std::wstring& s, const wchar_t* suf);
ComPtr<IShellItem> AppsFolderItem(const std::wstring& aumid, std::wstring* nameOut);
// apps_pins.cpp
std::wstring PackageFamily(const std::wstring& aumid);
// apps_click.cpp
std::wstring Describe(HWND h);
std::wstring InFront();
