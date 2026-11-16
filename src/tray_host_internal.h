#pragma once
// The tray host (tray_host.h) in parts, sharing what is declared here:
//   tray_host.cpp           the host window and its thread, messages passed on to Explorer, Start / Stop
//   tray_host_icons.cpp     what Shell_NotifyIcon sends: the icons, their pictures, clicks (Shown, Image, Send)
//   tray_host_settings.cpp  which icons Windows shows (NotifyIconSettings), an icon's lines for dock.log
//   tray_host_widget.cpp    hosting: a program's taskbar widget shown on the dock (SetHosting, Place)
#include "tray_host.h"
#include "explorer_taskbar.h"
#include "icon_pixels.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <mutex>

// Names a test can replace, so a host it runs is invisible to the real programs and settings.
#ifndef LD_TRAY_CLASS
#define LD_TRAY_CLASS L"Shell_TrayWnd"
#endif
#ifndef LD_TRAY_SETTINGS_KEY
#define LD_TRAY_SETTINGS_KEY L"Control Panel\\NotifyIconSettings"
#define LD_TRAY_SETTINGS_PARENT L"Control Panel"
#endif
#ifndef LD_TRAY_CREATED_MESSAGE
#define LD_TRAY_CREATED_MESSAGE L"TaskbarCreated"
#endif

// Whether this process runs as administrator (the test pretends either way)
#ifndef LD_TRAY_ELEVATED
#define LD_TRAY_ELEVATED trayhost::ProcessElevated()
#endif

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif
#ifndef NIN_SELECT
#define NIN_SELECT (WM_USER + 0)
#endif
#ifndef NIN_POPUPOPEN
#define NIN_POPUPOPEN (WM_USER + 6)
#endif
#ifndef NIN_POPUPCLOSE
#define NIN_POPUPCLOSE (WM_USER + 7)
#endif

namespace trayhost {

// ---------------------------------------------------------------- icons
struct Icon {
    uint64_t key = 0, seq = 0;
    HWND hwnd = nullptr; UINT uid = 0;
    bool hasGuid = false; GUID guid{};
    UINT callback = 0, version = 0;
    std::wstring tip; bool showTip = false;
    bool hidden = false;
    int w = 0, h = 0; std::vector<uint32_t> pixels; uint32_t imageVersion = 0; bool glyph = false;
    std::wstring exe;                     // lowercase path of the owning program
    bool promoted = false; int order = INT_MAX;
    std::wstring entry;                   // the NotifyIconSettings subkey that decided "promoted" (log)
    bool hasAnchor = false; RECT anchor{};
};

constexpr UINT kPlaceMsg = WM_APP + 0x2c1;   // thread message: wParam 0 placement / hosting, 1 announce

// ---------------------------------------------------------------- which icons Windows shows
// Windows 11 keeps one subkey per icon it has seen: ExecutablePath ("{known folder}\rest" or a full
// path), UID or IconGuid, IsPromoted (shown on the taskbar). UIOrderList holds the subkey names, as
// 64-bit numbers, in the order of the taskbar and the hidden-icons flyout.
constexpr wchar_t kSettingsKey[] = LD_TRAY_SETTINGS_KEY;

struct Entry {
    std::wstring name, exe;
    bool hasGuid = false; GUID guid{};
    bool hasUid = false; DWORD uid = 0;
    bool promoted = false; int order = INT_MAX;
};

// tray_host.cpp
extern std::atomic<bool> g_ready;
extern HANDLE g_thread;
extern DWORD g_threadId;
extern HWND g_wnd, g_notifyWnd;
extern std::atomic<bool> g_elevated;
HWND ExplorerTray();
bool Forward(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
// tray_host_icons.cpp
extern std::mutex g_mu;
extern std::vector<Icon> g_icons;
extern std::atomic<int> g_version;
extern std::vector<std::pair<HWND, std::wstring>> g_exeCache;
std::wstring Lower(std::wstring s);
LRESULT OnCopyData(WPARAM wp, LPARAM lp);
// tray_host_settings.cpp
extern std::vector<Entry> g_entries;
void LoadEntries();
void LogLines(const std::vector<std::wstring>& lines);
void DescribeIcon(const Icon& ic, const wchar_t* what, const wchar_t* picture, std::vector<std::wstring>& out);
void LogMissing(const std::vector<std::wstring>& arrived);
bool UpdatePromotion(Icon& ic);
bool UpdateAllPromotion();
// tray_host_widget.cpp
extern std::mutex g_hostedMu;
extern RECT g_hostedBox;
extern bool g_hostedFitted;
extern HWND g_hostedChild;
extern bool g_hostingApplied;
extern bool g_showing;
extern bool g_regionShown;
extern HRGN g_appliedRgn;
void UpdateHosted(const RECT* at = nullptr);
void StopRealign();
void ForgetWidget();
void ApplyPlacement();
void ApplyHosting();
void MirrorRect();

}   // namespace trayhost
