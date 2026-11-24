#pragma once
// The FluentFlyout module (fluent_flyout.h) in parts, sharing what is declared here:
//   fluent_flyout.cpp         FluentFlyout's widget window, its monitor, the module's entry points (Tick, WidgetState ...)
//   fluent_settings_file.cpp  FluentFlyout's settings.xml: where it is, read again when it changes
//   fluent_media.cpp          the media sessions ("something plays"), its app filter, the apps' names, the media thread
//   fluent_placement.cpp      its flyouts placed next to a shown dock
#include "fluent_flyout.h"
#include "fluent_logic.h"
#include "fluent_settings.h"
#include "tray_host.h"
#include "explorer_taskbar.h"
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <atomic>
#include <mutex>
#include <optional>

namespace fluentflyout {

using FfSettings = fluentsettings::Settings;

// A process's program as Windows gives it, empty when it cannot be read (the caller opens the process with
// the access it needs: fluent_flyout.cpp and fluent_media.cpp ask for different rights)
inline std::wstring ImagePath(HANDLE process) {
    wchar_t path[MAX_PATH * 2];
    DWORD n = ARRAYSIZE(path);
    std::wstring s;
    if (QueryFullProcessImageNameW(process, 0, path, &n)) s.assign(path, n);
    return s;
}

// fluent_settings_file.cpp
extern FfSettings g_ff;
extern int g_ffVersion;
extern int g_fileSetting;
extern double g_fileAt;
extern bool g_choiceDirty;
void LoadSettings(double now, bool force);
// fluent_media.cpp
extern std::atomic<bool> g_mediaWanted;
extern std::atomic<bool> g_namesWanted;
extern fluentlogic::Steady g_media;
void WantMedia(bool on);
void StopMedia();
bool FirstListPending(double now);
void UpdateHasMedia(double now);
// fluent_placement.cpp
void Unhook();
void Hook();
void ForgetFlyouts();
void PruneFlyouts();
// fluent_flyout.cpp
extern DWORD g_pid;

}   // namespace fluentflyout
