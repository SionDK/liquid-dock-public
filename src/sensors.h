#pragma once
#include "common.h"
#include "sensor_shared.h"
#include "sensor_logic.h"
#include <mutex>
#include <thread>
#include <atomic>

// The "PC 상태" panel's own readings (drives, memory modules, the GPU's memory, the footer's counts):
// sensor_logic.h, where the sensor thread's pass (hw::SensorThread, hw_panel.h) and its test build them
// without the dock.
using PanelData = sensorlogic::PanelData;

// System monitor data. Everything that needs no admin rights is sampled by the dock itself on a
// background thread: every 2 s (what the dock's cell shows), every second while a panel is open (and
// then the panel's drives and memory too). The CPU temperature comes from the one source the settings name
// (sensorSource, sensorlogic::ShowItems): HWiNFO when it reports one to its Gadget key (no driver needed
// here), or the elevated helper (PawnIO driver); none when temperatures are off.
class SensorClient {
public:
    enum Metric { CPU = 0, GPU, RAM, SSD, Count };

    void Start();                            // launch the local sampler
    void Tick(double now);
    // Once per main-loop pass, after every dock's Update: whether any dock shows (or is opening) its panel.
    // Closed -> open wakes the sampler for a full sample at once; open -> closed counts after 5 s.
    void SetPanelOpen(bool wanted, double now);
    bool Live() const { return live; }
    bool CpuTempAvailable() const { return live && (d.flags & LD_HAS_CPU_TEMP); }
    bool CpuTempFromHWiNFO() const { return live && (d.flags & LD_CPU_TEMP_HWINFO); }
    bool HWiNFORunning() const { return live && (d.flags & LD_HWINFO); }
    const LDSensorBlock& Data() const { return d; }
    const PanelData& Panel() const { return panel; }
    // How many drives and memory rows a dock window should leave room for, and whether for the VRAM block
    // (Dock::Metrics, when the window is built: its thickness is fixed then). Before the first opening a
    // count made once (UI thread, at the dock's start): the drives the panel's list would hold
    // (hw::CountPanelDrives: the same rules, StorageDeviceProperty only - no drive is woken; under 1 ms), the
    // installed modules (SMBIOS, read once for the process) and whether the display adapters promise a VRAM
    // block (hw::Video: an NVIDIA adapter, or 1 GB of dedicated memory on the adapter the dock draws with -
    // NVML itself is not loaded for it; DXGI's adapter list, about 0.2 ms in the dock, 6 - 8 ms in a process
    // without Direct3D). After it what the panel held, never less than that count: a later, lower count does not
    // change T (a Relayout would rebuild the window for it); a higher one does (rows otherwise left out).
    // What the sensor settings hide keeps no room (sensorlogic::ShownRoom: applied on top of that count).
    using PanelRoom = sensorlogic::PanelRoom;
    PanelRoom PanelRoomHint() const;
    // The sensor settings as the cell, the panel, the overheat alert and the peek use them (UI thread):
    // the source and the six items to show (sensorlogic::ShowItems)
    sensorlogic::ShowItems Items() const;
    // NVML gave the GPU memory's temperature at some panel sample (its row then stays without HWiNFO)
    bool VramNvmlTempSeen() const { return vramNvmlTempSeen; }
    float Temp(Metric m) const;             // NAN when unknown
    // ...NAN also when the settings hide it (Items: the CPU's temperature, the GPU, the drives): what the cell,
    // the cards, the overheat alert and the peek go by
    float ShownTemp(Metric m) const;
    float Threshold(Metric m) const { return m == CPU ? 90.f : m == GPU ? 83.f : m == SSD ? 70.f : 1e9f; }
    int Severity(Metric m) const;           // of ShownTemp: 0 normal, 1 warm (within 10 °C), 2 hot
    // the panel's drives and memory modules: hot at 70 °C (every drive, as the system drive) and 85 °C,
    // warm 10 °C below (60, 75); SeverityOf(row.temp, kDriveHot) -> 0 normal, 1 warm, 2 hot (NAN: 0).
    // The GPU's memory (its junction temperature): hot at 100 °C, warm from 90. Graphics memory is rated for
    // a case temperature of 95 °C (Micron's GDDR6X data sheet: TC 0 to +95 °C, +105 °C for the extended
    // grade); the junction reading sits a few degrees above the case, and cards are reported to slow down
    // at about 110 °C - a figure no data sheet at hand confirms. In normal use GDDR6/GDDR7 stays far below
    // (a GDDR7 card tested: 42 - 44 °C idle).
    static constexpr float kDriveHot = 70.f, kDimmHot = 85.f, kVramHot = 100.f;
    static int SeverityOf(float celsius, float hot) { return sensorlogic::Severity(celsius, hot); }
    int version = 0;                         // bumps on every new sample (the panel's readings)
    // What the dock's sensor cell shows now (DrawSensorCell draws from it): CPU and GPU, each its temperature
    // when known, otherwise its load, rounded as drawn, with the temperature's severity. cellVersion bumps
    // only when that changes: a resting dock redraws for a sample only then (Dock::Update), not every 2 s.
    sensorlogic::CellKey CellKeyNow() const;
    int cellVersion = 0;

    bool alertPending = false;               // consumed by the primary dock

    // The sensor source PawnIO, chosen by the user (the settings' 온도 읽기, the cell's menu): UI thread, right
    // after the click. Without PawnIO's service a box tells how to get it, and nothing changes; otherwise the
    // helper is installed when it is missing or not the dock's own copy (adminhelper::NeedsInstall: one UAC
    // prompt, declined - nothing changes) and sensorSource becomes 2 (saved). True when it did.
    bool EnablePawnIO(HWND owner);
    static bool PawnIOInstalled();
    // how to let HWiNFO report its CPU temperature to the dock (no driver, no admin rights)
    static void ShowHWiNFOGuide();

    // The elevated helper (CPU temperature, windows of programs run as administrator: admin_helper.h).
    static bool InstallHelper(HWND owner);   // one UAC prompt: copies it to the admin-only folder, registers its task
    static void StartHelper();               // worker thread: runs its scheduled task (no prompt; silent)

    // The helper's scheduled task is registered (schtasks /Query: a process, up to ~0.1 s - on a click only,
    // adminhelper::NeedsInstall)
    static bool HelperTaskRegistered();

private:
    void ReadHelper(double now);
    bool DropUnread(const sensorlogic::ShowItems& show);   // Tick: drops what the source in force does not read

    // local sampler
    std::thread sampler;
    std::mutex mu;
    LDSensorBlock localBlk{};
    PanelData localPanel;                    // under mu
    bool localPanelNew = false;              // under mu
    std::atomic<bool> localNew{ false };
    HANDLE wake = nullptr;                   // auto-reset: wakes the sampler when a panel opens
    std::atomic<bool> panelOpen{ false };    // the sampler's view of SetPanelOpen
    // the sampler's view of the settings (Tick): read HWiNFO (it is the source), ask the drives' temperatures
    std::atomic<bool> readHWiNFO{ true }, readDrives{ true };
    bool vramNvmlTempSeen = false;           // VramNvmlTempSeen
    sensorlogic::PanelLinger linger;

    // helper
    HANDLE map = nullptr;
    const LDSensorBlock* view = nullptr;
    LDSensorBlock helperBlk{};
    bool helperLive = false;
    HANDLE wantEvent = nullptr;              // LD_SENSOR_WANT while the source is PawnIO (ShowItems::WantsPawnIO)
    uint32_t lastSeq = 0;
    double lastHelperSample = 0, lastHelperTry = -1e9, lastPoll = 0;
    double helperRetry = 20.0;               // started again after that long: doubles while it does not come up (10 min at most)
    bool helperTried = false;                // started since it was last there

    LDSensorBlock d{};
    PanelData panel;
    bool live = false;
    sensorlogic::CellKey cellKey{};          // CellKeyNow as of the last cellVersion bump
    bool armed[Count] = { true, true, true, true };
    double lastAlert[Count] = { -1e9, -1e9, -1e9, -1e9 };
};

extern SensorClient g_sensors;
