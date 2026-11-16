// The dock's side of the elevated helper's sensor readings (src/sensors.cpp, SensorClient::Tick and ReadHelper):
// when it looks for the helper's shared memory, when it starts the helper and how often while that does not
// bring it up, and which readings it takes from it. The production file is compiled here with fakes for
// everything that would reach the real helper, the system or the user: the shared memory (OpenFileMappingW
// opens a block of this test's own), the want event (CreateEventW: a running helper would start sampling - the
// PawnIO driver - if this test made the real one), the start of the helper (TryDetach: counted, not run, so no
// schtasks.exe), PawnIO's service, the install's UAC prompt and the message boxes, and the clock the helper
// stamps its samples with (GetTickCount64: the simulated one). Simulated hours of the dock's Tick calls (4 a
// second); nothing is opened, started or shown.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\sensors_client_test.cpp src\hwinfo.cpp /link user32.lib advapi32.lib shell32.lib pdh.lib
#include "../src/common.h"
#include "../src/sensor_shared.h"
#include "../src/hw_sample.h"
#include "../src/hwinfo.h"
#include <shellapi.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <new>
#include <thread>
#define private public
#include "../src/sensors.h"
#undef private
#include "portable_check.h"

namespace fake {
int opens = 0, maps = 0, unmaps = 0, events = 0, starts = 0, processes = 0, installs = 0, boxes = 0, saves = 0;
bool helperUp = false;              // the helper's shared memory is there
bool pawnio = true;                 // PawnIO's service is installed
bool uacDeclined = false;           // the install's UAC prompt is answered No (ShellExecuteEx fails)
bool needsInstall = true;           // the installed helper is missing or not the dock's own copy (adminhelper::NeedsInstall)
int compares = 0;
std::wstring eventName;             // the want event the dock made
HANDLE block = nullptr;             // the "helper's" shared memory: a mapping of this test's own
LDSensorBlock* helper = nullptr;    // the helper's side of it (writable)
double now = 1000;
std::vector<std::wstring> logs;
}

static HANDLE WINAPI FakeOpenFileMapping(DWORD, BOOL, LPCWSTR name) {
    ++fake::opens;
    if (!fake::helperUp || wcscmp(name, LD_SENSOR_MAP) != 0) { SetLastError(ERROR_FILE_NOT_FOUND); return nullptr; }
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), fake::block, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return dup;
}
static LPVOID WINAPI FakeMapView(HANDLE h, DWORD access, DWORD high, DWORD low, SIZE_T bytes) {
    ++fake::maps;
    return MapViewOfFile(h, access, high, low, bytes);
}
static BOOL WINAPI FakeUnmap(LPCVOID view) { ++fake::unmaps; return UnmapViewOfFile(view); }
static HANDLE WINAPI FakeCreateEvent(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCWSTR name) {
    ++fake::events;
    fake::eventName = name ? name : L"";
    return CreateEventW(nullptr, manual, initial, nullptr);   // unnamed: the real helper never sees it
}
template <class F> bool FakeTryDetach(F&&) { ++fake::starts; return true; }   // the start counted, not run
static BOOL WINAPI FakeCreateProcess(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                     LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) {
    ++fake::processes;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}
static SC_HANDLE WINAPI FakeOpenSCManager(LPCWSTR, LPCWSTR, DWORD) { return (SC_HANDLE)(uintptr_t)1; }
static SC_HANDLE WINAPI FakeOpenService(SC_HANDLE, LPCWSTR, DWORD) { return fake::pawnio ? (SC_HANDLE)(uintptr_t)2 : nullptr; }
static BOOL WINAPI FakeCloseService(SC_HANDLE) { return TRUE; }
static BOOL WINAPI FakeShellExecuteEx(SHELLEXECUTEINFOW* sei) {
    if (fake::uacDeclined) { SetLastError(ERROR_CANCELLED); return FALSE; }
    ++fake::installs; sei->hProcess = nullptr; return TRUE;
}
static HINSTANCE WINAPI FakeShellExecute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) { return (HINSTANCE)(uintptr_t)33; }
static int WINAPI FakeMessageBox(HWND, LPCWSTR, LPCWSTR, UINT) { ++fake::boxes; return IDNO; }
static ULONGLONG WINAPI FakeTick() { return (ULONGLONG)(fake::now * 1000); }

#define OpenFileMappingW FakeOpenFileMapping
#define MapViewOfFile FakeMapView
#define UnmapViewOfFile FakeUnmap
#define CreateEventW FakeCreateEvent
#define TryDetach FakeTryDetach
#define CreateProcessW FakeCreateProcess
#define OpenSCManagerW FakeOpenSCManager
#define OpenServiceW FakeOpenService
#define CloseServiceHandle FakeCloseService
#define ShellExecuteExW FakeShellExecuteEx
#define ShellExecuteW FakeShellExecute
#define MessageBoxW FakeMessageBox
#define GetTickCount64 FakeTick
#include "../src/sensors.cpp"
#undef OpenFileMappingW
#undef MapViewOfFile
#undef UnmapViewOfFile
#undef CreateEventW
#undef TryDetach
#undef CreateProcessW
#undef OpenSCManagerW
#undef OpenServiceW
#undef CloseServiceHandle
#undef ShellExecuteExW
#undef ShellExecuteW
#undef MessageBoxW
#undef GetTickCount64

Settings g_settings;
void Settings::Load() {}
void Settings::Save() const { ++fake::saves; }
double NowSeconds() { return fake::now; }
void Log(const wchar_t* fmt, ...) {
    wchar_t line[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(line, 1024, fmt, ap);
    va_end(ap);
    fake::logs.emplace_back(line);
}
std::wstring FromUtf8(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::wstring SystemExe(const wchar_t* name) { return std::wstring(L"\"C:\\Windows\\System32\\") + name + L"\""; }
// the two helper files compared (admin_helper.cpp, not compiled here: no %ProgramFiles% read)
bool adminhelper::NeedsInstall() { ++fake::compares; return fake::needsInstall; }

namespace {

constexpr double kStep = 0.25;   // the dock's Tick looks every 0.25 s

struct Counts { int opens, maps, unmaps, starts; };
Counts Now() { return { fake::opens, fake::maps, fake::unmaps, fake::starts }; }

// The helper publishes one sample (its seqlock: odd while writing).
void Publish(float cpu, const wchar_t* status = L"") {
    LDSensorBlock* b = fake::helper;
    b->seq++;
    MemoryBarrier();
    b->magic = LD_SENSOR_MAGIC;
    b->size = sizeof(LDSensorBlock);
    b->flags = LD_HAS_CPU_TEMP;
    b->cpuTemp = cpu;
    b->tick = FakeTick();
    wcsncpy_s(b->status, status, _TRUNCATE);
    MemoryBarrier();
    b->seq++;
}

// The dock's own sampler has a new sample (its thread is not run here): no CPU temperature of its own.
void LocalSample(SensorClient& s) {
    LDSensorBlock b{};
    b.cpuLoad = 12;
    { std::lock_guard<std::mutex> lk(s.mu); s.localBlk = b; }
    s.localNew = true;
}

// `seconds` of Tick calls; the helper publishes every second while `publishing`.
void Run(SensorClient& s, double seconds, bool publishing = false) {
    const double end = fake::now + seconds;
    double nextPublish = fake::now;
    while (fake::now < end) {
        if (publishing && fake::now >= nextPublish) { Publish(55); nextPublish += 1.0; }
        s.Tick(fake::now);
        fake::now += kStep;
    }
}

void Fresh(SensorClient& s) {
    if (s.view) UnmapViewOfFile(s.view);
    if (s.map) CloseHandle(s.map);
    if (s.wantEvent) CloseHandle(s.wantEvent);
    if (s.wake) CloseHandle(s.wake);
    s.~SensorClient();
    new (&s) SensorClient();
    fake::opens = fake::maps = fake::unmaps = fake::events = fake::starts = 0;
    fake::helperUp = false;
    std::memset(fake::helper, 0, sizeof(LDSensorBlock));
}

void Hours() {
    SensorClient s;

    // the source HWiNFO (the default: CPU temperature from PawnIO off), no helper: the dock used to look for its
    // memory 4 times a second
    Fresh(s);
    g_settings.sensorSource = 1;
    Run(s, 3600);
    std::printf("  cpu temp off, no helper, 1 h: %d looks for the helper's memory, %d starts\n", fake::opens, fake::starts);
    Check(fake::opens == 0, "CPU temperature off, no helper: the dock does not look for the helper's memory (was 14,400 times an hour)");
    Check(fake::starts == 0 && fake::events == 0, "and neither starts the helper nor asks it for sensors");

    // CPU temperature off, the helper running for the windows of elevated programs
    Fresh(s);
    fake::helperUp = true;
    Publish(60);   // what it published long ago
    Run(s, 3600);
    LocalSample(s);
    Run(s, 1);
    Check(fake::opens == 0 && fake::maps == 0, "CPU temperature off, the helper up for windows only: its memory is not opened");
    Check(!s.CpuTempAvailable(), "and none of its readings is shown");

    // CPU temperature on, the helper up and publishing
    Fresh(s);
    g_settings.sensorSource = 2;
    fake::helperUp = true;
    Run(s, 2, true);
    LocalSample(s);
    Publish(57, L"");
    Run(s, 0.5);
    Check(fake::events == 1 && fake::eventName == LD_SENSOR_WANT, "CPU temperature on: the dock asks the helper for sensors (its want event)");
    Check(s.CpuTempAvailable() && s.Temp(SensorClient::CPU) == 57.f, "and shows the helper's CPU temperature");
    Counts before = Now();
    Run(s, 3600, true);
    std::printf("  cpu temp on, helper publishing, 1 h: %d opens, %d maps\n", fake::opens - before.opens, fake::maps - before.maps);
    Check(fake::opens == before.opens && fake::maps == before.maps && fake::starts == 0,
          "an hour with the helper up: its memory opened once, the helper not started");

    // turned off while the helper publishes: let go at once; on again: looked for at once, nothing stale shown
    g_settings.sensorSource = 1;
    Run(s, kStep);
    Publish(61);   // its last sample, after the dock let go (it stops within a second of the want event going)
    Check(fake::unmaps == before.unmaps + 1 && !s.view && !s.map && !s.helperLive,
          "CPU temperature turned off: the helper's memory is let go on the next Tick");
    LocalSample(s);
    Run(s, kStep);
    Check(!s.CpuTempAvailable(), "and its CPU temperature is no longer shown");
    before = Now();
    Run(s, 600);
    Check(fake::opens == before.opens, "and its memory is not looked for while it is off");
    g_settings.sensorSource = 2;
    Run(s, kStep);
    LocalSample(s);
    Run(s, kStep);
    Check(fake::opens == before.opens + 1 && s.view, "turned on again: the helper's memory is opened on the next Tick");
    Check(!s.CpuTempAvailable(), "and no sample from before it was turned off is shown again");
    Check(!s.helperLive, "(nor the one it published after the dock let go: 10 min old)");
    Publish(58);
    LocalSample(s);
    Run(s, kStep);
    Check(s.CpuTempAvailable() && s.Temp(SensorClient::CPU) == 58.f, "its next sample is");

    // the dock starts while the helper's block holds a sample of long ago (an earlier dock's): not shown; the
    // helper's first new sample is, at once
    Fresh(s);
    g_settings.sensorSource = 2;
    fake::helperUp = true;
    Publish(62);
    fake::now += 3600;
    Run(s, 2);
    LocalSample(s);
    Run(s, kStep);
    Check(s.view && !s.helperLive && !s.CpuTempAvailable(), "the dock starts, the helper's last sample is an hour old: not shown");
    Check(fake::starts == 0, "and the helper is not started again (its memory is there: it is up)");
    Publish(63);
    LocalSample(s);
    Run(s, kStep);
    Check(s.helperLive && s.Temp(SensorClient::CPU) == 63.f, "its next sample is shown on the next Tick");
    Run(s, 4.75, true);
    Check(s.helperLive, "and the ones after, a second apart (new by its clock: taken)");
}

// CPU temperature on, the helper never comes up (its task deleted, say): how often the dock starts it.
void Starts() {
    SensorClient s;
    Fresh(s);
    g_settings.sensorSource = 2;
    std::vector<double> at;
    const double t0 = fake::now;
    int seen = 0;
    while (fake::now < t0 + 7200) {
        s.Tick(fake::now);
        if (fake::starts != seen) { seen = fake::starts; at.push_back(fake::now - t0); }
        fake::now += kStep;
    }
    int firstHour = 0;
    for (double t : at) if (t < 3600) ++firstHour;
    const int secondHour = (int)at.size() - firstHour;
    std::printf("  helper never up: starts at");
    for (size_t i = 0; i < at.size() && i < 8; ++i) std::printf(" %.2f", at[i]);
    std::printf(" ... s; %d in the first hour, %d in the second\n", firstHour, secondHour);
    Check(!at.empty() && at[0] == 0, "CPU temperature on, no helper: started on the first Tick, as before");
    Check(at.size() > 1 && at[1] > 20 && at[1] <= 20.5, "the second start 20 s after the first, as before");
    Check(at.size() > 3 && at[2] - at[1] > 40 && at[2] - at[1] <= 40.5 && at[3] - at[2] > 80 && at[3] - at[2] <= 80.5,
          "then twice as long each time it does not come up (40 s, 80 s, ...)");
    bool capped = true;
    for (size_t i = 1; i < at.size(); ++i) if (at[i] - at[i - 1] > 600.5) capped = false;
    Check(capped && secondHour == 6, "at most 10 min apart: 6 starts an hour once it has grown (was 178)");
    Check(firstHour <= 10, "10 starts in the first hour (was 178: one every 20 s)");
    Check(fake::processes == 0, "(no schtasks.exe was run by this test)");

    // the helper comes up at last; later it dies: started again at once, and 20 s later, as before
    fake::helperUp = true;
    Run(s, 60, true);   // up for a minute: the last start is longer ago than any wait
    Check(s.helperLive, "the helper comes up: its readings are taken");
    fake::helperUp = false;   // it dies: no more samples, and its memory is gone for anyone who opens it now
    const int startsBefore = fake::starts;
    const double lastSample = s.lastHelperSample;
    double first = -1, second = -1;
    while (fake::now < lastSample + 40) {
        s.Tick(fake::now);
        if (fake::starts == startsBefore + 1 && first < 0) first = fake::now - lastSample;
        if (fake::starts == startsBefore + 2 && second < 0) second = fake::now - lastSample;
        fake::now += kStep;
    }
    std::printf("  helper died: started again %.2f s and %.2f s after its last sample\n", first, second);
    Check(first > 5 && first <= 5.5, "it dies: started again once its last sample is 5 s old, as before");
    Check(second - first > 20 && second - first <= 20.5, "and again 20 s later (what it had grown to is forgotten once it was up)");

    // turned on again by the user (the install starts it): the next start 20 s later, not what it had grown to
    Fresh(s);
    g_settings.sensorSource = 2;
    Run(s, 3000);   // grown to 10 min
    g_settings.sensorSource = 1;
    fake::pawnio = true;
    const int installs = fake::installs, saves = fake::saves;
    Check(s.EnablePawnIO(nullptr) && fake::installs == installs + 1 && g_settings.sensorSource == 2 && fake::saves == saves + 1 && fake::boxes == 0,
          "the source PawnIO chosen: the helper installed (one UAC prompt), sensorSource 2 saved");
    const int afterInstall = fake::starts;
    const double installed = fake::now;
    double next = -1;
    while (fake::now < installed + 60 && next < 0) {
        s.Tick(fake::now);
        if (fake::starts > afterInstall) next = fake::now - installed;
        fake::now += kStep;
    }
    std::printf("  after the install: the dock starts it %.2f s later\n", next);
    Check(next > 20 && next <= 20.5, "the install starts it: the dock starts it again 20 s later if it is not up, as before");

    // chosen again with the dock's own copy installed (the same bytes): no UAC prompt, started through its task at once
    Fresh(s);
    g_settings.sensorSource = 2;
    Run(s, 3000);   // grown to 10 min
    g_settings.sensorSource = 1;
    Run(s, 1);
    fake::needsInstall = false;
    const int compares = fake::compares, installs2 = fake::installs, saves2 = fake::saves, before = fake::starts;
    Check(s.EnablePawnIO(nullptr) && fake::compares == compares + 1 && fake::installs == installs2 && g_settings.sensorSource == 2 &&
          fake::saves == saves2 + 1, "the same helper installed: PawnIO chosen without an install (no UAC prompt), sensorSource 2 saved");
    const double chosen = fake::now;
    double firstStart = -1, secondStart = -1;
    while (fake::now < chosen + 60 && secondStart < 0) {
        s.Tick(fake::now);
        if (fake::starts == before + 1 && firstStart < 0) firstStart = fake::now - chosen;
        if (fake::starts == before + 2 && secondStart < 0) secondStart = fake::now - chosen;
        fake::now += kStep;
    }
    std::printf("  the same helper: started %.2f s and %.2f s after the choice\n", firstStart, secondStart);
    Check(firstStart >= 0 && firstStart <= 0.5, "...started through its task on the next look (not after what it had grown to)");
    Check(secondStart - firstStart > 20 && secondStart - firstStart <= 20.5, "...and again 20 s later if it is not up");
    fake::needsInstall = true;
}

// A local sample is taken on the Tick it arrives (only the helper's memory waits for the 0.25 s step), and the
// panel's open state: the sampler woken when a panel opens, kept sampling 5 s after it closes; the panel's data
// copied with the sample it came with and kept between openings.
void Panel() {
    SensorClient s;
    Fresh(s);
    g_settings.sensorSource = 2;
    fake::helperUp = true;
    Run(s, 1, true);
    const int opens = fake::opens, version = s.version;
    LocalSample(s);
    s.Tick(s.lastPoll + 0.05);   // 50 ms after the last look at the helper's memory
    Check(s.version == version + 1 && !s.localNew, "a local sample 50 ms after the last Tick: taken on that Tick (it used to wait for the 0.25 s step)");
    Check(fake::opens == opens && s.lastPoll < fake::now, "...without looking at the helper's memory again before its 0.25 s");
    Check(s.helperLive && s.Temp(SensorClient::CPU) == 55.f, "...the helper's CPU temperature still merged into it");

    // the wake event (made by Start: an auto-reset one of the test's own here)
    s.wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto woken = [&] { return WaitForSingleObject(s.wake, 0) == WAIT_OBJECT_0; };
    double t = 5000;
    s.SetPanelOpen(false, t);
    Check(!s.panelOpen && !woken(), "no panel: the sampler samples every 2 s, not woken");
    s.SetPanelOpen(true, t += 0.016);
    Check(s.panelOpen && woken(), "a dock opens its panel: every second from now, and the sampler woken for a full sample at once");
    for (int i = 0; i < 30; ++i) s.SetPanelOpen(true, t += 0.016);
    Check(s.panelOpen && !woken(), "the panel stays open: woken once only");
    s.SetPanelOpen(false, t += 0.016);
    const double closed = t;
    while (t < closed + 4.98) s.SetPanelOpen(false, t += 0.016);
    Check(s.panelOpen, "closed: still sampled every second for 5 s");
    s.SetPanelOpen(false, t += 0.05);
    Check(!s.panelOpen && !woken(), "...then every 2 s again (no wake)");
    s.SetPanelOpen(true, t += 1);
    Check(s.panelOpen && woken(), "opened again: woken again");
    s.SetPanelOpen(false, t += 1);
    s.SetPanelOpen(true, t += 2);
    Check(s.panelOpen && !woken(), "closed and opened again within 5 s: no second wake (it was still sampling every second)");
    // without the event (it could not be made) nothing breaks: the sampler sleeps its period out
    CloseHandle(s.wake);
    s.wake = nullptr;
    s.SetPanelOpen(false, t += 10);
    s.SetPanelOpen(true, t += 0.016);
    Check(s.panelOpen, "no wake event: the panel still counts as open");

    // the panel's data: copied with the sample it came with, kept while samples come without it
    Check(!s.Panel().sampled && s.Panel().drives.empty(), "before any panel sample: nothing, sampled false");
    {
        PanelData p;
        sensorlogic::DriveRow c; c.letter = L'C'; c.disk = 0; c.system = true; c.model = L"Samsung MZVL21T0HCLR-00B00"; c.temp = 39;
        p.drives.push_back(c);
        p.drivesWithTemp = 1; p.modules = 2; p.sampled = true;
        p.dimms = sensorlogic::BuildDimmRows({}, {});
        p.vram.present = true; p.vram.usedGB = 1.861f; p.vram.totalGB = 7.960f; p.vram.load = 23.4f; p.vram.temp = 44.0f;
        std::lock_guard<std::mutex> lk(s.mu);
        s.localPanel = p;
        s.localPanelNew = true;
    }
    LocalSample(s);
    s.Tick(fake::now);
    Check(s.Panel().sampled && s.Panel().drives.size() == 1 && s.Panel().drives[0].model == L"Samsung MZVL21T0HCLR-00B00" && s.Panel().modules == 2,
          "a sample with the panel's data: it is Panel() from that Tick");
    Check(s.Panel().vram.present && s.Panel().vram.usedGB == 1.861f && s.Panel().vram.totalGB == 7.960f && s.Panel().vram.temp == 44.0f,
          "...with its VRAM block (1.9 of 8.0 GB, 44.0 °C)");
    LocalSample(s);
    fake::now += 2;
    s.Tick(fake::now);
    Check(s.Panel().sampled && s.Panel().drives.size() == 1 && s.Panel().vram.present, "a sample without (the panel closed): the last panel data stays");
    Check(SensorClient::kVramHot == 100.f && SensorClient::SeverityOf(89.9f, SensorClient::kVramHot) == 0 && SensorClient::SeverityOf(90.f, SensorClient::kVramHot) == 1 &&
          SensorClient::SeverityOf(99.9f, SensorClient::kVramHot) == 1 && SensorClient::SeverityOf(100.f, SensorClient::kVramHot) == 2,
          "the GPU memory's temperature: hot at 100 °C, warm from 90 (the modules' 85 and the drives' 70 are not its)");
}

// The sensor cell's version: every sample bumps version (the panel shows each), cellVersion only when the
// cell would look different (the dock redraws for it; sensor_logic.h CellKey) - the helper's CPU temperature
// merged into the sample counts, and so does going live.
void Cell() {
    SensorClient s;
    Fresh(s);
    g_settings.sensorSource = 1;
    auto sample = [&](uint32_t flags, float cpuLoad, float gpuTemp = 0, float gpuLoad = 0) {
        LDSensorBlock b{};
        b.flags = flags; b.cpuLoad = cpuLoad; b.gpuTemp = gpuTemp; b.gpuLoad = gpuLoad;
        { std::lock_guard<std::mutex> lk(s.mu); s.localBlk = b; }
        s.localNew = true;
        fake::now += 2;
        s.Tick(fake::now);
    };
    Check(!s.Live() && s.version == 0 && s.cellVersion == 0, "before the first sample: nothing, no cell version");
    sample(LD_HAS_GPU | LD_HAS_GPU_LOAD, 12, 40.2f, 30);
    Check(s.Live() && s.version == 1 && s.cellVersion == 1, "the first sample (going live): both versions bump");
    sample(LD_HAS_GPU | LD_HAS_GPU_LOAD, 12, 40.2f, 30);
    Check(s.version == 2 && s.cellVersion == 1, "the same sample again: version bumps, the cell's does not");
    sample(LD_HAS_GPU | LD_HAS_GPU_LOAD, 12.4f, 39.6f, 85);
    Check(s.version == 3 && s.cellVersion == 1, "CPU load 12.4 % (12%), GPU 39.6 °C (40°) and its load 85 %: the cell looks the same");
    sample(LD_HAS_GPU | LD_HAS_GPU_LOAD, 12.6f, 39.6f, 85);
    Check(s.version == 4 && s.cellVersion == 2, "CPU load 12.6 % (13%, no CPU temperature): the cell changes");
    sample(LD_HAS_GPU_LOAD, 12.6f, 39.6f, 85);
    Check(s.cellVersion == 3, "the GPU's temperature gone (its load shown): the cell changes");
    sample(LD_HAS_GPU_LOAD, 12.6f, 39.6f, 85);
    Check(s.cellVersion == 3, "...and stays");

    // the helper's CPU temperature, merged into each local sample
    Fresh(s);
    g_settings.sensorSource = 2;
    fake::helperUp = true;
    Run(s, 1, true);   // up, publishing 55 °C; no local sample yet
    Check(!s.Live() && s.cellVersion == 0, "the helper up without a local sample: nothing taken yet");
    auto helper = [&](float cpu, float cpuLoad) {
        Publish(cpu);
        LDSensorBlock b{};
        b.cpuLoad = cpuLoad;
        { std::lock_guard<std::mutex> lk(s.mu); s.localBlk = b; }
        s.localNew = true;
        fake::now += 0.25;   // the helper's memory looked at on this Tick
        s.Tick(fake::now);
    };
    helper(55.2f, 12);
    Check(s.Live() && s.Temp(SensorClient::CPU) == 55.2f && s.cellVersion == 1, "the first sample with the helper's 55.2 °C: the cell changes");
    int v = s.version;
    helper(55.4f, 80);
    Check(s.version == v + 1 && s.cellVersion == 1, "55.4 °C and the load 12 -> 80 % (a temperature known): the cell looks the same");
    helper(55.6f, 80);
    Check(s.cellVersion == 2, "the helper's CPU temperature 55.4 -> 55.6 °C (55° -> 56°): the cell changes");
    helper(89.6f, 80);
    Check(s.cellVersion == 3 && s.Severity(SensorClient::CPU) == 1, "89.6 °C (90°, warm): a change");
    helper(90.0f, 80);
    Check(s.cellVersion == 4 && s.Severity(SensorClient::CPU) == 2, "90.0 °C (90° again, now hot - red): a change");
    helper(90.2f, 80);
    Check(s.cellVersion == 4, "90.2 °C (90°, hot): the same");
    s.alertPending = false;
    g_settings.sensorSource = 1;
}

// The sensor settings (sensorSource, the six show switches; sensor_logic.h ShowItems): which source is read
// and asked, what the cell's key holds, what alerts.
void Sources() {
    SensorClient s;
    auto local = [&](uint32_t flags, float cpuTemp, float cpuLoad, float gpuTemp, float ssdTemp = 0) {
        LDSensorBlock b{};
        b.flags = flags; b.cpuTemp = cpuTemp; b.cpuLoad = cpuLoad; b.gpuTemp = gpuTemp; b.gpuLoad = 20; b.ssdTemp = ssdTemp;
        { std::lock_guard<std::mutex> lk(s.mu); s.localBlk = b; }
        s.localNew = true;
        fake::now += 0.25;
        s.Tick(fake::now);
    };
    const uint32_t hwinfoCpu = LD_HAS_CPU_TEMP | LD_CPU_TEMP_HWINFO | LD_HWINFO;

    // HWiNFO (1): read by the sampler, the helper not asked
    Fresh(s);
    g_settings = Settings{};
    Check(g_settings.sensorSource == 1 && g_settings.showCpuTemp && g_settings.showGpu && g_settings.showVram && g_settings.showVramTemp &&
          g_settings.showRamTemp && g_settings.showSsd, "the defaults: the source HWiNFO Gadget, every item shown");
    Run(s, 60);
    local(hwinfoCpu | LD_HAS_GPU | LD_HAS_GPU_LOAD | LD_HAS_SSD_TEMP, 52, 10, 41, 39);
    Check(s.readHWiNFO && s.readDrives && fake::events == 0 && fake::opens == 0, "HWiNFO: the sampler reads it and the drives; the helper is neither asked nor looked for");
    Check(s.Temp(SensorClient::CPU) == 52.f && s.CpuTempFromHWiNFO() && s.HWiNFORunning(), "...its CPU temperature shown");

    // off (0): nothing of HWiNFO, no helper; a pass the sampler took before the change counts for nothing
    g_settings.sensorSource = 0;
    local(hwinfoCpu | LD_HAS_GPU | LD_HAS_GPU_LOAD | LD_HAS_SSD_TEMP, 52, 10, 41, 39);
    Check(!s.readHWiNFO && s.readDrives, "off: the sampler's next pass does not read HWiNFO (no registry or process polling)");
    Check(!s.CpuTempAvailable() && !s.HWiNFORunning() && std::isnan(s.Temp(SensorClient::CPU)), "...and a pass with HWiNFO's reading taken before: not shown");
    Check(s.Temp(SensorClient::GPU) == 41.f && s.Temp(SensorClient::SSD) == 39.f, "...the GPU (NVML) and the drive (IOCTL) need neither: shown");
    Run(s, 60);
    Check(fake::events == 0 && fake::opens == 0 && !s.wantEvent, "...no want event, the helper's memory not looked for");
    {
        const auto k = s.CellKeyNow();
        Check(k.cpu.kind == 2 && k.cpu.value == 10 && k.gpuLine && k.gpu.kind == 1 && k.gpu.value == 41, "...the cell: CPU's load (10%), the GPU's temperature");
    }

    // PawnIO (2): the helper's CPU temperature alone (HWiNFO not read, its reading of a pass before not taken)
    g_settings.sensorSource = 2;
    fake::helperUp = true;
    Run(s, 2, true);
    local(hwinfoCpu, 52, 10, 0);
    Publish(61);
    local(hwinfoCpu, 52, 10, 0);
    Check(!s.readHWiNFO && fake::events == 1 && fake::eventName == LD_SENSOR_WANT && s.helperLive, "PawnIO: HWiNFO not read, the helper asked (its want event) and up");
    Check(s.Temp(SensorClient::CPU) == 61.f && !s.CpuTempFromHWiNFO(), "...the CPU temperature the helper's 61 °C, not HWiNFO's 52 (there is no \"HWiNFO wins\" any more)");
    // its CPU temperature switched off: the helper is not asked (it lets the driver go), the cell shows the load
    g_settings.showCpuTemp = false;
    Run(s, 0.5);   // the next look at the helper (0.25 s after the last)
    Check(!s.wantEvent && !s.view && !s.helperLive, "PawnIO with the CPU temperature hidden: the helper no longer asked, its memory let go");
    local(LD_HAS_GPU | LD_HAS_GPU_LOAD, 0, 37.4f, 44);
    Check(s.CellKeyNow().cpu.kind == 2 && s.CellKeyNow().cpu.value == 37 && std::isnan(s.ShownTemp(SensorClient::CPU)), "...the cell: the CPU's load (37%)");
    g_settings.showCpuTemp = true;
    fake::helperUp = false;
    Run(s, kStep);

    // the source PawnIO chosen without PawnIO's service: the box, nothing changes; with the UAC prompt declined: nothing changes
    g_settings.sensorSource = 1;
    fake::pawnio = false;
    int installs = fake::installs, boxes = fake::boxes, saves = fake::saves;
    Check(!s.EnablePawnIO(nullptr) && g_settings.sensorSource == 1 && fake::boxes == boxes + 1 && fake::installs == installs && fake::saves == saves,
          "PawnIO chosen without its driver: a box says how to get it, the source stays HWiNFO, nothing saved");
    fake::pawnio = true;
    fake::uacDeclined = true;
    boxes = fake::boxes;
    Check(!s.EnablePawnIO(nullptr) && g_settings.sensorSource == 1 && fake::boxes == boxes && fake::saves == saves,
          "PawnIO chosen, the UAC prompt declined: the source stays HWiNFO, nothing saved");
    fake::uacDeclined = false;

    // the cell's key: the GPU hidden - no GPU line (none, kind 0), the key changes (a redraw)
    Fresh(s);
    g_settings = Settings{};
    local(hwinfoCpu | LD_HAS_GPU | LD_HAS_GPU_LOAD, 47, 10, 41);
    int cv = s.cellVersion;
    g_settings.showGpu = false;
    local(hwinfoCpu | LD_HAS_GPU | LD_HAS_GPU_LOAD, 47, 10, 41);
    {
        const auto k = s.CellKeyNow();
        Check(!k.gpuLine && k.gpu == sensorlogic::CellReading{} && k.cpu.kind == 1 && k.cpu.value == 47 && s.cellVersion == cv + 1,
              "the GPU hidden: the cell's key has no GPU line (its reading none) - a change, the cell redrawn");
    }
    Check(std::isnan(s.ShownTemp(SensorClient::GPU)) && s.Severity(SensorClient::GPU) == 0 && s.Temp(SensorClient::GPU) == 41.f,
          "...its temperature is not shown (nor its severity), though still read");
    // the CPU temperature hidden: its load
    g_settings.showGpu = true; g_settings.showCpuTemp = false;
    local(hwinfoCpu | LD_HAS_GPU | LD_HAS_GPU_LOAD, 47, 10, 41);
    Check(s.CellKeyNow().cpu.kind == 2 && s.CellKeyNow().gpuLine && s.CellKeyNow().gpu.kind == 1, "the CPU temperature hidden: the cell shows its load, the GPU as before");
    g_settings.showCpuTemp = true;

    // the drives hidden: not asked, the system drive's temperature not shown
    g_settings.showSsd = false;
    local(hwinfoCpu | LD_HAS_SSD_TEMP, 47, 10, 0, 75);
    Check(!s.readDrives && std::isnan(s.Temp(SensorClient::SSD)), "SSD hidden: the sampler does not ask the drives; a temperature of a pass before is not shown");
    g_settings.showSsd = true;

    // the overheat alert: only what is shown
    Fresh(s);
    g_settings = Settings{};
    g_settings.showGpu = false; g_settings.showSsd = false; g_settings.showCpuTemp = false;
    s.alertPending = false;
    local(hwinfoCpu | LD_HAS_GPU | LD_HAS_SSD_TEMP, 95, 10, 90, 75);
    Check(!s.alertPending, "CPU, GPU and SSD hidden, all three over their thresholds (95, 90, 75 °C): no overheat alert");
    g_settings.showGpu = true;
    local(hwinfoCpu | LD_HAS_GPU | LD_HAS_SSD_TEMP, 95, 10, 90, 75);
    Check(s.alertPending, "the GPU shown again (90 °C, hot at 83): its alert");
    s.alertPending = false;
    g_settings.showCpuTemp = true; g_settings.sensorSource = 0;
    local(hwinfoCpu, 95, 10, 0);
    Check(!s.alertPending, "temperatures off: no CPU temperature (HWiNFO's 95 °C of a pass before) - no alert");
    s.alertPending = false;

    // the GPU memory's temperature from NVML: seen once, it keeps its row without HWiNFO
    Fresh(s);
    g_settings = Settings{};
    Check(!s.VramNvmlTempSeen(), "no panel sample yet: NVML's VRAM temperature not seen");
    {
        PanelData p; p.sampled = true; p.vram.present = true; p.vram.totalGB = 16; p.vram.temp = 50; p.vram.nvmlTemp = true;
        std::lock_guard<std::mutex> lk(s.mu);
        s.localPanel = p; s.localPanelNew = true;
    }
    local(0, 0, 10, 0);
    Check(s.VramNvmlTempSeen(), "a panel sample with NVML's VRAM temperature: seen (and kept)");

    // the source switched between two samples: the old one's CPU temperature goes on the next Tick (the cell
    // redrawn), not with the next sample up to 2 s later
    Fresh(s);
    g_settings = Settings{};
    local(hwinfoCpu | LD_HAS_GPU | LD_HAS_GPU_LOAD, 47, 10, 41);
    cv = s.cellVersion;
    g_settings.sensorSource = 2;
    fake::now += kStep; s.Tick(fake::now);   // no new sample of the dock's own
    Check(!s.CpuTempAvailable() && !s.CpuTempFromHWiNFO() && !s.HWiNFORunning() && s.CellKeyNow().cpu.kind == 2 && s.cellVersion == cv + 1,
          "HWiNFO -> PawnIO between two samples: HWiNFO's 47 °C gone at once (the cell shows the load, redrawn)");
    fake::helperUp = true;
    Run(s, 2, true);
    local(LD_HAS_GPU | LD_HAS_GPU_LOAD, 0, 10, 41);
    Check(s.Temp(SensorClient::CPU) == 55.f && !s.CpuTempFromHWiNFO(), "(PawnIO: the helper's 55 °C)");
    cv = s.cellVersion;
    g_settings.sensorSource = 1;
    fake::now += kStep; s.Tick(fake::now);
    Check(!s.CpuTempAvailable() && s.CellKeyNow().cpu.kind == 2 && s.cellVersion == cv + 1,
          "PawnIO -> HWiNFO between two samples: the helper's 55 °C gone at once");
    cv = s.cellVersion;
    fake::now += kStep; s.Tick(fake::now);
    Check(s.cellVersion == cv, "...and nothing more goes on the Ticks after (no redraw)");
    fake::helperUp = false;

    // the overheat alert re-arms by what is read, shown or not: cooled while hidden, shown again and hot - an alert
    Fresh(s);
    g_settings = Settings{};
    s.alertPending = false;
    local(LD_HAS_GPU, 0, 10, 90);
    Check(s.alertPending, "(the GPU at 90 °C: its alert)");
    s.alertPending = false;
    g_settings.showGpu = false;
    local(LD_HAS_GPU, 0, 10, 60);
    local(LD_HAS_GPU, 0, 10, 90);
    Check(!s.alertPending, "the GPU hidden, cooled to 60 °C and hot again: no alert while hidden");
    g_settings.showGpu = true;
    local(LD_HAS_GPU, 0, 10, 90);
    Check(s.alertPending, "...shown again (within 10 min): its alert - it cooled down meanwhile (re-armed)");
    s.alertPending = false;
    g_settings = Settings{};
}

}   // namespace

int main() {
    fake::block = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LDSensorBlock), nullptr);
    fake::helper = fake::block ? (LDSensorBlock*)MapViewOfFile(fake::block, FILE_MAP_WRITE, 0, 0, sizeof(LDSensorBlock)) : nullptr;
    if (!fake::helper) { std::printf("FAIL: no memory of its own for the helper's block\n"); return 1; }
    Hours();
    Starts();
    Panel();
    Cell();
    Sources();
    return Summary("sensors_client_test");
}
