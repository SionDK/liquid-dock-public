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

struct Flyout {
    HWND h = nullptr;
    enum State { Waiting, Moving, Placed, Left } state = Waiting;
    double shownAt = 0, movedAt = 0, settledAt = 0;
    bool aligned = false, raised = false, nearShownDock = false;
    RECT from{}, to{}, expect{};
    double moveStart = 0;
};

// fluent_settings_file.cpp
extern FfSettings g_ff;
extern int g_ffVersion;
extern int g_fileSetting;
extern double g_fileAt;
extern bool g_choiceDirty;
extern int g_fileMonitor;
extern std::atomic<bool> g_namesWanted;
void LoadSettings(double now, bool force);
// fluent_media.cpp
extern std::atomic<bool> g_mediaWanted;
extern HANDLE g_mediaThread, g_mediaStop;
extern fluentlogic::Steady g_media;
void WantMedia(bool on);
bool FirstListPending(double now);
void UpdateHasMedia(double now);
// fluent_placement.cpp
extern std::vector<Flyout> g_flyouts;
extern std::unordered_map<HMONITOR, int> g_lift;
void Unhook();
void Hook();
// fluent_flyout.cpp
extern DWORD g_pid;

}   // namespace fluentflyout
