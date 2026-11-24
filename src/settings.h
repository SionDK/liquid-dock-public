#pragma once
#include <string>

enum class Edge { Bottom = 0, Left = 1, Right = 2, Top = 3 };
inline bool IsVertical(Edge e) { return e == Edge::Left || e == Edge::Right; }

struct Settings {
    Edge  edge = Edge::Bottom;
    float iconSize = 44;        // at a 1440 px short screen side; scaled with the screen
    float magnification = 1.7f; // 1 = off
    int   glassStyle = 1;       // 0 clear, 1 normal, 2 frosted
    std::string trayButtons = "desktop,theme,sff";   // the user's buttons between the sensor cell and settings, in order
    // read only from older files (before trayButtons), to build trayButtons once
    bool  showDesktopButton = true, showThemeButton = true, showOledButton = false;
    bool  oledOneMonitor = false;   // black wallpaper only on the monitor whose dock was clicked
    bool  trayIcons = false;    // experimental: the icons Windows shows on its taskbar, next to the chevron (tray_host.h)
    bool  fluentFlyout = true;  // FluentFlyout, when it runs: its taskbar widget left of start, its flyouts next to the dock (fluent_flyout.h)
    int   clockSize = 0;        // 0 normal, 1 large; independent of icon size
    int   clockFormat = 0;      // clock time: 0 as Windows shows it (its 12 or 24 hour setting), 1 24 hours ("15:45")
    int   sensorSize = 0;       // CPU/GPU temperature text on the dock: 0 normal, 1 large
    int   iconSpacing = 1;      // 0 compact, 1 normal, 2 wide (menu presets)
    int   iconGap = 0;          // gap between icons in % of the icon (settings slider); 0 = use iconSpacing
    int   dockThickness = 100;  // glass thickness in % of the default; icons keep their size
    int   edgeGap = 6;          // px between the screen edge and the glass (at a 1440 px short side)
    int   edgeOffset = 0;       // along the edge: -100 left/top end, 0 centred, 100 right/bottom end
    bool  autoHide = true;      // false: always shown, with its strip of the screen reserved (not in OLED mode)
    bool  secondaryAlwaysShown = false;   // the docks on the other (non-primary) monitors stay shown even with autoHide
    bool  showWithStart = true; // a hidden dock comes up with Start, search, quick settings and notifications
    float showDelay = 0.05f;    // seconds at edge before reveal
    float hideDelay = 0.35f;    // seconds outside before hide
    bool  autostart = false;
    bool  startMenuExtras = true;   // right-click on the start button: power and system menu
    bool  shiftRunsAsAdmin = true;  // Shift+click on an app icon runs it as administrator
    bool  taskbarAppButtons = true; // an app with one window, or a pinned one not running: its click presses the app's own button on Windows' (hidden) taskbar (taskbar_apps.h)
    int   revealHotkey = 0;         // 0 off, 1 Ctrl+Alt+D, 2 Ctrl+Shift+Space, 3 Win+Shift+D
    // where temperatures are read (sensorlogic::ShowItems): 0 off, 1 HWiNFO Gadget, 2 PawnIO (the elevated helper)
    int   sensorSource = 1;
    bool  cpuTemp = false;      // read only from older files (before sensorSource): 1 there is sensorSource 2 (PawnIO)
    bool  sourceFromCpuTemp = false;   // not saved: Load took sensorSource 2 from such a cpuTemp=1 (SensorClient::Start settles it)
    // what the "PC 상태" panel and the dock's sensor cell show (the 센서 page's "PC 상태에 보일 항목")
    bool  showCpuTemp = true, showGpu = true, showVram = true, showVramTemp = true, showRamTemp = true, showSsd = true;
    // the panel's power draw (the 센서 page's "전력 소비량", sensorlogic::ShowItems): 0 없음 (nothing read), 1 그래프 (the
    // cards' last 60 s), 2 요약 (the footer's watts); under 1 and 2 the CPU's (HWiNFO only) and the GPU's each on or off
    int   powerView = 0;
    bool  showCpuPower = true, showGpuPower = true;
    int   adminWindows = 0;     // windows of programs run as administrator through the elevated helper (admin_helper.h): 0 not asked yet, 1 on, 2 off
    std::string hwinfoCpuLabel; // HWiNFO reading to use for the CPU temperature, e.g. "CPU (Tctl/Tdie)"; empty: automatic
    int   pinsVersion = 0;      // 2 = store-app pins imported
    void Load();
    void Save() const;
};
extern Settings g_settings;   // the one copy, defined in main.cpp
