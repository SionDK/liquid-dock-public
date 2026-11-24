#pragma once
// What the parts of the app model share (apps.cpp, apps_pins.cpp, apps_click.cpp, apps_press.cpp); each is
// defined once, in the file named.
#include "apps.h"
#include <shobjidl.h>   // IShellItem
#include "main.h"   // g_mainWnd: woken when a taskbar button's press has its result (the loop may be asleep)
// apps.cpp
std::wstring ProcessExe(DWORD pid);
bool IsCloaked(HWND h);
DWORD CloakOf(HWND h);   // DWMWA_CLOAKED as Windows gives it: 0, or who cloaked it (1 the app, 2 the shell, 4 inherited)
bool IsTaskbarWindow(HWND h);
std::wstring FileStem(const std::wstring& path);
bool EndsWithI(const std::wstring& s, const wchar_t* suf);
// 'the same app' as Refresh and PinFile ask it (each in its own order: AppModel::ItemFor)
bool SameAppId(const std::wstring& itemId, const std::wstring& id);
bool SameProgramOneIdless(const std::wstring& itemExe, const std::wstring& itemId, const std::wstring& exe, const std::wstring& id);
ComPtr<IShellItem> AppsFolderItem(const std::wstring& aumid, std::wstring* nameOut);
std::wstring AppsFolderTarget(const std::wstring& aumid);   // L"shell:AppsFolder\\" + aumid
void ReadAumid(IPropertyStore* ps, std::wstring& out);
// a shortcut opened (OpenShortcut); its file interface kept with the link, let go before it as each reader did
struct Shortcut { ComPtr<IShellLinkW> link; ComPtr<IPersistFile> file; };
bool OpenShortcut(const std::wstring& lnk, Shortcut& s);
// apps_pins.cpp
std::wstring PackageFamily(const std::wstring& aumid);
// apps_click.cpp
std::wstring Describe(HWND h);
std::wstring InFront();
bool IsItOrItsOwn(HWND fg, HWND h);
void MinimizeWindow(HWND h, const std::wstring& name);   // the admin helper's way, else WM_SYSCOMMAND SC_MINIMIZE
