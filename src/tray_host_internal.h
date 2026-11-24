#pragma once
// The tray host (tray_host.h) in parts, sharing what is declared here:
//   tray_host.cpp           the host window and its thread, messages passed on to Explorer, Start / Stop
//   tray_host_icons.cpp     what Shell_NotifyIcon sends: the icons, their pictures, clicks (Shown, Image, Send)
//   tray_host_settings.cpp  which icons Windows shows (NotifyIconSettings), an icon's lines for dock.log
//   tray_host_widget.cpp    hosting: a program's taskbar widget shown on the dock (SetHosting, Place)
#include "tray_host.h"
#include "elevation.h"
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
#define LD_TRAY_ELEVATED ProcessElevated()
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
// An icon the dock shows: on the taskbar in Windows' settings, not hidden by its program, with a picture.
inline bool ShownOnDock(const Icon& ic) { return ic.promoted && !ic.hidden && ic.w > 0; }

constexpr UINT kPlaceMsg = WM_APP + 0x2c1;   // thread message from the dock (wParam: one of the two below)
constexpr WPARAM kPlaceHosting = 0;          // kPlaceMsg: apply the dock's placement and hosting
constexpr WPARAM kPlaceAnnounce = 1;         // kPlaceMsg: ask programs to re-add their icons (Announce)

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
bool ForwardAppBar(WPARAM wp, LPARAM lp, LRESULT& result);   // WM_COPYDATA dwData 0: its shared block made for Explorer (tray_host.cpp)
// tray_host_icons.cpp
extern std::mutex g_mu;
extern std::vector<Icon> g_icons;
extern std::atomic<int> g_version;
extern std::vector<std::pair<HWND, std::wstring>> g_exeCache;
std::wstring Lower(std::wstring s);
std::wstring ImagePath(HANDLE process);   // the process's program as Windows gives it (not lowered), empty when unread
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
// The host thread's own of the hosting. Each part is cleared where it always was, never all at once:
// ApplyHosting clears regionShown and coveredLogged, EndHosting hostingApplied, regionShown and
// appliedRgn, ForgetWidget the widget's region and the realigning. shownLogged stays when a host stops
// (a widget window is described in the log once), showing is set only around one SetWindowPos.
struct Realigner;
struct HostedWidget {
    bool hostingApplied = false;
    bool showing = false;                 // this SetWindowPos may show the window
    bool regionShown = false;             // the region shows the hosted widget (else empty)
    HRGN appliedRgn = nullptr;            // a copy of the window's region
    HWND shownLogged = nullptr;           // the hosted window last described in the log
    bool coveredLogged = false;           // "the dock came over it" logged in this hosting
    HRGN widgetRgn = nullptr;             // the shown widget's region as last read...
    HWND widgetRgnOf = nullptr;           // ...of this window
    double widgetRgnAt = -1;              // ...then
    HWND realignOf = nullptr;             // the widget last put back at this window's corner
    POINT realignOff{};                   // ...from this offset
    double realignAt = -1;                // ...then
    int realigned = 0;                    // ...how often while running (for the log)
    Realigner* realigner = nullptr;       // the host thread makes it and the thread
    HANDLE realignThread = nullptr;
};
extern HostedWidget g_hostedWidget;       // host thread
void UpdateHosted(const RECT* at = nullptr);
void StopRealign();
void EndHosting();
void ApplyPlacement();
void ApplyHosting();
void MirrorRect();

}   // namespace trayhost
