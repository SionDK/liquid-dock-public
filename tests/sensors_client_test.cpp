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
#include <system_error>
#include <type_traits>
#define private public
#include "../src/sensors.h"
#undef private
#include "portable_check.h"
// sensors.cpp's other headers, before the fakes below rename anything (none of them calls a renamed function)
#include "../src/hw_panel.h"
#include "../src/admin_helper.h"
#include <cstddef>

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

// The helper's install and start path and SensorClient::Start, as calls in order (tracing: on in those tests
// only, main thread only). Nothing real is run: the exe's path is the fake's, the helper file is asked of the
// fake, a "process" is a pair of events of this test's own, and Start's sampler thread is a fake that parks.
namespace fake {
bool tracing = false;
std::vector<std::wstring> trace;
DWORD mainThread = 0;
void Trace(const std::wstring& s) { if (tracing && GetCurrentThreadId() == mainThread) trace.push_back(s); }
std::wstring modulePath = L"C:\\Users\\dock\\AppData\\Local\\Programs\\LiquidDock\\LiquidDock.exe";   // GetModuleFileNameW
DWORD moduleSize = 0;               // the buffer it was given (characters)
bool helperFile = false;            // GetFileAttributesW finds the helper's file
bool processOk = false;             // CreateProcessW "starts" one (else: it fails)
std::wstring processArgs;           // what it was asked with besides the command line
HANDLE process = nullptr, thread = nullptr;   // the "process" handles it gave out (events of this test's own)
DWORD waitResult = WAIT_OBJECT_0;   // what the wait on it answers
bool exitCodeOk = true;             // GetExitCodeProcess answers
DWORD exitCode = 0;
std::wstring seVerb, seFile, seParams;   // ShellExecuteExW, as asked
int seShow = -1;
ULONG seMask = 0;
HWND seOwner = nullptr;
DWORD seSize = 0;
bool seProcess = false;             // it gives a process handle back
HANDLE seHandle = nullptr;
bool eventFails = false;            // CreateEventW fails (ERROR_NOT_ENOUGH_MEMORY)
bool readerFound = false;           // the probe's HWiNFO CPU temperature (hwinfo::Reader::ReadCpu().found)
std::wstring samplerLabel;          // the label the sampler thread's reader was made with
HANDLE samplerUp = nullptr;         // set when Start's sampler thread is up
// what Settings::Save saw when it was called (the file gets what the settings hold then)
int savedSource = -99;
int terminates = 0;                 // TerminateProcess calls (none expected: a timed-out schtasks is left running)
// Start's sampler loop run for a few passes (SamplerLoop): 0 (the default) parks the pass object in Init
int samplerPasses = 0;
int panelOnPass = 0;                // SamplePanel answers true on this pass
int powerOnPass = -1;               // SamplePower answers true on this pass (0: every pass, -1: never)
struct PowerWant { int view = -1; bool cpu = false, gpu = false; } powerWant;   // the last WantPower
bool threadFails = false;           // std::thread cannot be made (Start's catch)
std::mutex samplerMu;
std::vector<std::wstring> samplerTrace;   // the sampler thread's calls, in order (under samplerMu)
HANDLE samplerPassDone = nullptr, samplerGoOn = nullptr;   // after each pass: done set, then go on awaited
thread_local bool onSampler = false;
void STrace(const std::wstring& s) { if (!onSampler) return; std::lock_guard<std::mutex> lk(samplerMu); samplerTrace.push_back(s); }
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
    fake::Trace(std::wstring(L"CreateEventW ") + (name ? name : L"(unnamed)") + (manual ? L" manual" : L" auto") + (initial ? L" set" : L" unset"));
    if (fake::eventFails) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    return CreateEventW(nullptr, manual, initial, nullptr);   // unnamed: the real helper never sees it
}
template <class F> bool FakeTryDetach(F&&) { ++fake::starts; return true; }   // the start counted, not run
static BOOL WINAPI FakeCreateProcess(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL inherit, DWORD flags, LPVOID env,
                                     LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    ++fake::processes;
    if (!fake::processOk) {
        fake::Trace(std::wstring(L"CreateProcessW ") + (cmd ? cmd : L"(none)") + L" -> fails");
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    wchar_t args[256];
    swprintf_s(args, L"app=%ls inherit=%d flags=0x%08lX env=%ls cwd=%ls cb=%lu si.flags=0x%lX show=%u", app ? app : L"null", inherit, flags,
               env ? L"set" : L"null", cwd ? cwd : L"null", si ? si->cb : 0ul, si ? si->dwFlags : 0ul, si ? (unsigned)si->wShowWindow : 99u);
    fake::processArgs = args;
    fake::Trace(std::wstring(L"CreateProcessW ") + (cmd ? cmd : L"(none)"));
    fake::process = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // no process: two events of this test's own
    fake::thread = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pi->hProcess = fake::process; pi->hThread = fake::thread; pi->dwProcessId = 4242; pi->dwThreadId = 4243;
    return TRUE;
}
static DWORD WINAPI FakeWaitForSingleObject(HANDLE h, DWORD ms) {
    if (fake::onSampler) fake::STrace(L"wait wake " + std::to_wstring(ms));   // the sampler's wait between passes (its wake event)
    if (!h || h != fake::process) return WaitForSingleObject(h, ms);
    fake::Trace(L"WaitForSingleObject " + std::to_wstring(ms) + (fake::waitResult == WAIT_TIMEOUT ? L" -> timeout" : L""));
    return fake::waitResult;
}
static BOOL WINAPI FakeGetExitCodeProcess(HANDLE h, LPDWORD code) {
    if (!fake::exitCodeOk || h != fake::process) { fake::Trace(L"GetExitCodeProcess -> fails"); return FALSE; }
    fake::Trace(L"GetExitCodeProcess -> " + std::to_wstring(fake::exitCode));
    *code = fake::exitCode;
    return TRUE;
}
static BOOL WINAPI FakeCloseHandle(HANDLE h) {
    if (h && h == fake::process) { fake::Trace(L"CloseHandle process"); fake::process = nullptr; }
    else if (h && h == fake::thread) { fake::Trace(L"CloseHandle thread"); fake::thread = nullptr; }
    else if (h && h == fake::seHandle) { fake::Trace(L"CloseHandle install process"); fake::seHandle = nullptr; }
    return CloseHandle(h);
}
static DWORD WINAPI FakeGetModuleFileName(HMODULE, LPWSTR out, DWORD size) {
    fake::moduleSize = size;
    fake::Trace(L"GetModuleFileNameW");
    const std::wstring& p = fake::modulePath;
    if (p.size() >= size) {   // as Windows does it: cut to size - 1 characters, terminated, ERROR_INSUFFICIENT_BUFFER
        wmemcpy(out, p.c_str(), size - 1);
        out[size - 1] = 0;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    wmemcpy(out, p.c_str(), p.size() + 1);
    return (DWORD)p.size();
}
static DWORD WINAPI FakeGetFileAttributes(LPCWSTR path) {
    fake::Trace(std::wstring(L"GetFileAttributesW ") + path);
    if (!fake::helperFile) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_FILE_ATTRIBUTES; }
    return FILE_ATTRIBUTE_ARCHIVE;
}
static SC_HANDLE WINAPI FakeOpenSCManager(LPCWSTR, LPCWSTR, DWORD) { fake::Trace(L"OpenSCManagerW"); return (SC_HANDLE)(uintptr_t)1; }
static SC_HANDLE WINAPI FakeOpenService(SC_HANDLE, LPCWSTR name, DWORD) {
    fake::Trace(std::wstring(L"OpenServiceW ") + (name ? name : L"(none)"));
    return fake::pawnio ? (SC_HANDLE)(uintptr_t)2 : nullptr;
}
static BOOL WINAPI FakeCloseService(SC_HANDLE) { return TRUE; }
static BOOL WINAPI FakeShellExecuteEx(SHELLEXECUTEINFOW* sei) {
    fake::seVerb = sei->lpVerb ? sei->lpVerb : L"(none)";
    fake::seFile = sei->lpFile ? sei->lpFile : L"(none)";
    fake::seParams = sei->lpParameters ? sei->lpParameters : L"(none)";
    fake::seShow = sei->nShow; fake::seMask = sei->fMask; fake::seOwner = sei->hwnd; fake::seSize = sei->cbSize;
    fake::Trace(L"ShellExecuteExW " + fake::seVerb + L" " + fake::seFile + L" " + fake::seParams + (fake::uacDeclined ? L" -> declined" : L""));
    if (fake::uacDeclined) { SetLastError(ERROR_CANCELLED); return FALSE; }
    ++fake::installs;
    sei->hProcess = fake::seProcess ? (fake::seHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr)) : nullptr;
    return TRUE;
}
static BOOL WINAPI FakeTerminateProcess(HANDLE h, UINT code) {   // never the real one
    ++fake::terminates;
    fake::Trace(std::wstring(L"TerminateProcess ") + (h == fake::process ? L"process" : L"other") + L" " + std::to_wstring(code));
    return TRUE;
}
static void WINAPI FakeSleep(DWORD ms) {
    if (fake::onSampler) fake::STrace(L"sleep " + std::to_wstring(ms));
    Sleep(ms);
}
static HINSTANCE WINAPI FakeShellExecute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) { return (HINSTANCE)(uintptr_t)33; }
static int WINAPI FakeMessageBox(HWND, LPCWSTR, LPCWSTR, UINT) { ++fake::boxes; return IDNO; }
static ULONGLONG WINAPI FakeTick() { return (ULONGLONG)(fake::now * 1000); }

// SensorClient::Start's HWiNFO reader (the settings' migration probe, and the sampler thread's) and the sampler
// thread's pass object: no registry read (hwinfo.cpp's Reader reads the real HKCU), no NVML / PDH / drives. The
// thread's pass object parks in Init: Start runs as it is, its thread never samples.
namespace hwinfo {
struct FakeReader {
    explicit FakeReader(std::wstring preferLabel) {
        if (GetCurrentThreadId() == fake::mainThread) fake::Trace(L"Reader(" + preferLabel + L")");
        else fake::samplerLabel = preferLabel;   // read by the test after samplerUp
    }
    FakeReader(const FakeReader&) = delete;
    CpuReading ReadCpu() {
        fake::Trace(L"ReadCpu");
        CpuReading c;
        c.running = c.found = fake::readerFound;
        c.celsius = fake::readerFound ? 47.f : 0.f;
        return c;
    }
};
}
namespace hw {
template <class R> class FakeSensorThread {
public:
    explicit FakeSensorThread(R&) {}
    void Init() {
        passes = fake::samplerPasses;
        fake::onSampler = passes > 0;
        fake::STrace(L"Init");
        SetEvent(fake::samplerUp);
        if (passes == 0) for (;;) Sleep(INFINITE);   // parked: nothing is sampled
    }
    // (SamplerLoop) each pass after the first starts here: the one before is done - the test looks, then lets it go on
    DWORD WaitMs(bool open) {
        fake::STrace(std::wstring(L"WaitMs open=") + (open ? L"1" : L"0"));
        if (pass > 0) {
            SetEvent(fake::samplerPassDone);
            if (pass >= passes) for (;;) Sleep(INFINITE);   // its passes run: parked
            WaitForSingleObject(fake::samplerGoOn, INFINITE);
        }
        ++pass;
        return open ? 1 : 2;   // short: the wait is a real one (on the wake event, or Sleep)
    }
    void Want(bool hwinfo, bool drives) {
        fake::STrace(std::wstring(L"Want hwinfo=") + (hwinfo ? L"1" : L"0") + L" drives=" + (drives ? L"1" : L"0"));
    }
    // the power draw (not traced: the order of the other calls is what the traces check). What the settings said
    // last (WantPower), and a sample on the passes fake::powerOnPass names (every pass when it is 0)
    void WantPower(int view, bool cpu, bool gpu) { fake::powerWant = { view, cpu, gpu }; }
    void SampleCell(bool open, ULONGLONG tick, LDSensorBlock& b) {
        fake::STrace(std::wstring(L"SampleCell open=") + (open ? L"1" : L"0") + L" tick=" + std::to_wstring(tick));
        b.flags = LD_HAS_GPU_LOAD; b.cpuLoad = 10.f + pass; b.gpuLoad = (float)pass;
    }
    bool SamplePower(bool, ULONGLONG tick, sensorlogic::PowerData& pw) {
        if (fake::powerOnPass < 0 || (fake::powerOnPass > 0 && pass != fake::powerOnPass)) return false;
        pw.view = fake::powerWant.view; pw.atMs = tick;
        if (fake::powerWant.cpu) { pw.cpu.read = true; pw.cpu.watts = 71.f; pw.cpu.history = { { tick - 1000, 70.f }, { tick, 71.f } }; pw.cpu.top = 80.f; }
        if (fake::powerWant.gpu) { pw.gpu.read = true; pw.gpu.watts = 287.f; pw.gpu.limitW = 360.f; pw.gpu.top = 360.f; }
        return true;
    }
    bool SamplePanel(bool open, ULONGLONG tick, PanelData& p) {
        fake::STrace(std::wstring(L"SamplePanel open=") + (open ? L"1" : L"0") + L" tick=" + std::to_wstring(tick));
        if (pass != fake::panelOnPass) return false;
        p.sampled = true; p.modules = 7; p.drivesWithTemp = pass;
        return true;
    }
private:
    int passes = 0, pass = 0;
};
}
// Start's thread could not be had (std::system_error, as std::thread throws it): sensors.cpp's `std::thread(...)`
// is renamed to this maker (a test-only addition to namespace std: the call site spells std::)
namespace std {
template <class F> std::thread FakeThreadMaker(F&& f) {
    if (fake::threadFails) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    return std::thread(std::forward<F>(f));
}
}

#define Reader FakeReader
#define SensorThread FakeSensorThread
#define WaitForSingleObject FakeWaitForSingleObject
#define GetExitCodeProcess FakeGetExitCodeProcess
#define CloseHandle FakeCloseHandle
#define GetModuleFileNameW FakeGetModuleFileName
#define GetFileAttributesW FakeGetFileAttributes
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
#define TerminateProcess FakeTerminateProcess
#define Sleep FakeSleep
#define thread FakeThreadMaker
#include "../src/sensors.cpp"
// the helper's install and start (HelperPath, RunHidden; sensors.cpp calls them), under the same fakes: no runas,
// no schtasks.exe, no helper run
#include "../src/admin_helper_process.cpp"
#undef TerminateProcess
#undef Sleep
#undef thread
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
#undef Reader
#undef SensorThread
#undef WaitForSingleObject
#undef GetExitCodeProcess
#undef CloseHandle
#undef GetModuleFileNameW
#undef GetFileAttributesW

Settings g_settings;
void Settings::Load() {}
void Settings::Save() const { ++fake::saves; fake::savedSource = sensorSource; fake::Trace(L"Save"); }
double NowSeconds() { return fake::now; }
void Log(const wchar_t* fmt, ...) {
    wchar_t line[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(line, 1024, fmt, ap);
    va_end(ap);
    fake::logs.emplace_back(line);
    fake::Trace(std::wstring(L"Log ") + line);
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
    fake::savedSource = -99;
    Check(s.EnablePawnIO(nullptr) && fake::installs == installs + 1 && g_settings.sensorSource == 2 && fake::saves == saves + 1 && fake::boxes == 0,
          "the source PawnIO chosen: the helper installed (one UAC prompt), sensorSource 2 saved");
    Check(fake::savedSource == 2, "char: ...what Save wrote: source 2 (set before it was saved)");
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
    fake::savedSource = -99;
    Check(s.EnablePawnIO(nullptr) && fake::compares == compares + 1 && fake::installs == installs2 && g_settings.sensorSource == 2 &&
          fake::saves == saves2 + 1, "the same helper installed: PawnIO chosen without an install (no UAC prompt), sensorSource 2 saved");
    Check(fake::savedSource == 2, "char: ...what Save wrote without an install: source 2 (set before it was saved)");
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
// The power draw on the UI thread (SensorClient::Tick, DropUnshownPower): what the settings show goes to the
// sampler before each pass, what they no longer show leaves on the Tick they change - a pass taken before that
// included -, and powerVersion bumps for every change (the panel's graph geometry is built again only then).
void PowerDrops() {
    SensorClient s;
    Fresh(s);
    g_settings = Settings{};
    g_settings.powerView = sensorlogic::kPowerGraph;
    auto sample = [&](uint64_t at) {   // a 그래프 pass with both devices, handed over as the sampler does
        sensorlogic::PowerData p;
        p.view = sensorlogic::kPowerGraph; p.atMs = at;
        p.cpu.read = p.gpu.read = true; p.cpu.watts = 71.f; p.gpu.watts = 287.f; p.gpu.limitW = 360.f;
        p.cpu.history = { { at - 1000, 69.f }, { at, 71.f } }; p.cpu.top = 80.f;
        p.gpu.history = { { at - 1000, 280.f }, { at, 287.f } }; p.gpu.top = 360.f;
        LDSensorBlock b{};
        std::lock_guard<std::mutex> lk(s.mu);
        s.localBlk = b; s.localPower = std::move(p); s.localPowerNew = true;
        s.localNew = true;
    };
    s.Tick(fake::now);
    Check(s.powerView == sensorlogic::kPowerGraph && s.powerCpu && s.powerGpu, "power: 그래프, the source HWiNFO - the sampler told to read both");
    Check(s.powerVersion == 0 && !s.Power().cpu.read, "...nothing to show before its first pass");
    sample(9000000);
    s.Tick(fake::now += kStep);
    const int v1 = s.powerVersion;
    Check(v1 == 1 && s.Power().cpu.watts == 71.f && s.Power().gpu.history.size() == 2 && s.Power().atMs == 9000000,
          "...a pass handed over: taken on the next Tick, powerVersion 1");
    s.Tick(fake::now += kStep);
    Check(s.powerVersion == v1, "...a Tick without a new pass: powerVersion stays (no geometry built again)");

    g_settings.powerView = sensorlogic::kPowerSummary;
    s.Tick(fake::now += kStep);
    Check(s.powerView == sensorlogic::kPowerSummary && s.powerVersion == v1 + 1 && s.Power().cpu.history.empty() && s.Power().gpu.history.empty() &&
          s.Power().cpu.top == 0 && s.Power().cpu.watts == 71.f && s.Power().gpu.limitW == 360.f,
          "요약: the histories and their tops go on that Tick (the current watts stay), powerVersion bumps once");
    sample(9001000);   // a 그래프 pass the sampler took before it saw the change
    s.Tick(fake::now += kStep);
    Check(s.Power().cpu.history.empty() && s.Power().gpu.top == 0 && s.Power().gpu.watts == 287.f,
          "...a 그래프 pass taken before the change: its histories dropped on the Tick it arrives");

    g_settings.showGpuPower = false;
    const int v2 = s.powerVersion;
    s.Tick(fake::now += kStep);
    Check(!s.powerGpu && s.powerCpu && !s.Power().gpu.read && std::isnan(s.Power().gpu.watts) && std::isnan(s.Power().gpu.limitW) &&
          s.Power().cpu.read && s.powerVersion == v2 + 1, "GPU 전력 off: the GPU's all gone at once (not the CPU's), the sampler told");
    g_settings.showGpuPower = true; g_settings.showGpu = false;
    s.Tick(fake::now += kStep);
    Check(!s.powerGpu, "the GPU hidden (its card gone): no GPU power read, whatever its switch");
    g_settings.showGpu = true;

    g_settings.sensorSource = sensorlogic::kSourceOff;
    s.Tick(fake::now += kStep);
    Check(!s.powerCpu && s.powerGpu && !s.Power().cpu.read && std::isnan(s.Power().cpu.watts),
          "온도 읽기 끄기: no CPU power (HWiNFO's only) - dropped at once, the GPU's still asked for");
    g_settings.sensorSource = sensorlogic::kSourcePawnIO;
    s.Tick(fake::now += kStep);
    Check(!s.powerCpu && s.powerGpu, "PawnIO: no CPU power either (the helper reads no RAPL)");

    g_settings.sensorSource = sensorlogic::kSourceHWiNFO;
    g_settings.powerView = sensorlogic::kPowerOff;
    sample(9002000);
    s.Tick(fake::now += kStep);
    Check(s.powerView == sensorlogic::kPowerOff && !s.powerCpu && !s.powerGpu && !s.Power().cpu.read && !s.Power().gpu.read &&
          s.Power().cpu.history.empty(), "없음: nothing asked for, and a pass that still arrives shows nothing");
    const int v3 = s.powerVersion;
    for (int i = 0; i < 40; ++i) s.Tick(fake::now += kStep);
    Check(s.powerVersion == v3, "...ten seconds of Ticks with nothing to drop: powerVersion stays");
    g_settings = Settings{};
}

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
    int sv = s.version;
    g_settings.sensorSource = 2;
    fake::now += kStep; s.Tick(fake::now);   // no new sample of the dock's own
    Check(!s.CpuTempAvailable() && !s.CpuTempFromHWiNFO() && !s.HWiNFORunning() && s.CellKeyNow().cpu.kind == 2 && s.cellVersion == cv + 1,
          "HWiNFO -> PawnIO between two samples: HWiNFO's 47 °C gone at once (the cell shows the load, redrawn)");
    Check(s.version == sv + 1, "char: ...version bumped once on that Tick, no sample (the panel redraws at once)");
    fake::helperUp = true;
    Run(s, 2, true);
    local(LD_HAS_GPU | LD_HAS_GPU_LOAD, 0, 10, 41);
    Check(s.Temp(SensorClient::CPU) == 55.f && !s.CpuTempFromHWiNFO(), "(PawnIO: the helper's 55 °C)");
    cv = s.cellVersion;
    sv = s.version;
    g_settings.sensorSource = 1;
    fake::now += kStep; s.Tick(fake::now);
    Check(!s.CpuTempAvailable() && s.CellKeyNow().cpu.kind == 2 && s.cellVersion == cv + 1,
          "PawnIO -> HWiNFO between two samples: the helper's 55 °C gone at once");
    Check(s.version == sv + 1, "char: ...version bumped once on that Tick, no sample");
    cv = s.cellVersion;
    sv = s.version;
    fake::now += kStep; s.Tick(fake::now);
    Check(s.cellVersion == cv, "...and nothing more goes on the Ticks after (no redraw)");
    Check(s.version == sv, "char: ...nor a version bump");
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

// ---------------------------------------------------------------- characterization: what is so now
// The shared memory's layout (src/sensor_shared.h): written by LiquidDockSensors.exe, read by the dock - two
// programs built apart, installed apart (the helper only when its bytes differ). The dock checks only the magic,
// never `size`, so these are the only guard of that agreement. x64, as both are built.
static_assert(sizeof(LDSensorBlock) == 720, "LDSensorBlock: 720 bytes");
static_assert(alignof(LDSensorBlock) == 8, "LDSensorBlock: 8-byte aligned (its tick)");
static_assert(offsetof(LDSensorBlock, magic) == 0 && offsetof(LDSensorBlock, size) == 4 && offsetof(LDSensorBlock, seq) == 8 &&
              offsetof(LDSensorBlock, flags) == 12 && offsetof(LDSensorBlock, tick) == 16, "LDSensorBlock: the header");
static_assert(offsetof(LDSensorBlock, cpuTemp) == 24 && offsetof(LDSensorBlock, cpuLoad) == 28, "LDSensorBlock: the CPU");
static_assert(offsetof(LDSensorBlock, gpuTemp) == 32 && offsetof(LDSensorBlock, gpuLoad) == 36 && offsetof(LDSensorBlock, gpuMemUsed) == 40 &&
              offsetof(LDSensorBlock, gpuMemTotal) == 44, "LDSensorBlock: the GPU");
static_assert(offsetof(LDSensorBlock, ramUsed) == 48 && offsetof(LDSensorBlock, ramTotal) == 52 && offsetof(LDSensorBlock, ramLoad) == 56,
              "LDSensorBlock: the memory");
static_assert(offsetof(LDSensorBlock, ssdTemp) == 60 && offsetof(LDSensorBlock, ssdActive) == 64 && offsetof(LDSensorBlock, ssdUsed) == 68 &&
              offsetof(LDSensorBlock, ssdTotal) == 72, "LDSensorBlock: the system drive");
static_assert(offsetof(LDSensorBlock, cpuName) == 76 && offsetof(LDSensorBlock, gpuName) == 204 && offsetof(LDSensorBlock, ssdName) == 332 &&
              offsetof(LDSensorBlock, status) == 460, "LDSensorBlock: the names and the status");
static_assert(sizeof(LDSensorBlock::cpuName) == 128 && sizeof(LDSensorBlock::gpuName) == 128 && sizeof(LDSensorBlock::ssdName) == 128 &&
              sizeof(LDSensorBlock::status) == 256 && sizeof(wchar_t) == 2, "LDSensorBlock: 64, 64, 64 and 128 UTF-16 characters");
static_assert(sizeof(LDSensorBlock::tick) == 8 && sizeof(LDSensorBlock::seq) == 4 && sizeof(LDSensorBlock::cpuTemp) == 4, "LDSensorBlock: field widths");
static_assert(LD_SENSOR_MAGIC == 0x4C445332u, "the magic: 'LDS2'");
// the fields' types: a same-sized type change (float -> int32_t, a volatile seq) passes the offsets
static_assert(std::is_same_v<decltype(LDSensorBlock::magic), uint32_t> && std::is_same_v<decltype(LDSensorBlock::size), uint32_t> &&
              std::is_same_v<decltype(LDSensorBlock::seq), volatile uint32_t> && std::is_same_v<decltype(LDSensorBlock::flags), uint32_t> &&
              std::is_same_v<decltype(LDSensorBlock::tick), uint64_t>, "LDSensorBlock: the header's types");
static_assert(std::is_same_v<decltype(LDSensorBlock::cpuTemp), float> && std::is_same_v<decltype(LDSensorBlock::cpuLoad), float> &&
              std::is_same_v<decltype(LDSensorBlock::gpuTemp), float> && std::is_same_v<decltype(LDSensorBlock::gpuLoad), float> &&
              std::is_same_v<decltype(LDSensorBlock::gpuMemUsed), float> && std::is_same_v<decltype(LDSensorBlock::gpuMemTotal), float> &&
              std::is_same_v<decltype(LDSensorBlock::ramUsed), float> && std::is_same_v<decltype(LDSensorBlock::ramTotal), float> &&
              std::is_same_v<decltype(LDSensorBlock::ramLoad), float> && std::is_same_v<decltype(LDSensorBlock::ssdTemp), float> &&
              std::is_same_v<decltype(LDSensorBlock::ssdActive), float> && std::is_same_v<decltype(LDSensorBlock::ssdUsed), float> &&
              std::is_same_v<decltype(LDSensorBlock::ssdTotal), float>, "LDSensorBlock: the readings are floats");
static_assert(std::is_same_v<decltype(LDSensorBlock::cpuName), wchar_t[64]> && std::is_same_v<decltype(LDSensorBlock::gpuName), wchar_t[64]> &&
              std::is_same_v<decltype(LDSensorBlock::ssdName), wchar_t[64]> && std::is_same_v<decltype(LDSensorBlock::status), wchar_t[128]>,
              "LDSensorBlock: the names are wchar_t arrays");
static_assert(LD_HAS_CPU_TEMP == 1 && LD_HAS_GPU == 2 && LD_HAS_SSD_TEMP == 4 && LD_HAS_GPU_LOAD == 8 && LD_HWINFO == 16 && LD_CPU_TEMP_HWINFO == 32,
              "the flags");

std::wstring Joined() {
    std::wstring s;
    for (const auto& t : fake::trace) s += (s.empty() ? L"" : L" | ") + t;
    return s;
}
void CheckTrace(const std::wstring& want, const char* what) {
    const std::wstring got = Joined();
    if (!Check(got == want, what)) {
        std::printf("  trace: %ls\n  want:  %ls\n", got.c_str(), want.c_str());
    }
}

void Layout() {
    Check(wcscmp(LD_SENSOR_MAP, L"Local\\LiquidDockSensors") == 0 && wcscmp(LD_SENSOR_TASK, L"LiquidDockSensors") == 0 &&
          wcscmp(LD_SENSOR_WANT, L"Local\\LiquidDock.WantSensors") == 0,
          "char: the helper's names: memory Local\\LiquidDockSensors, task LiquidDockSensors, want event Local\\LiquidDock.WantSensors");
    Check(LD_SENSOR_MAGIC == 0x4C445332u && sizeof(LDSensorBlock) == 720 && offsetof(LDSensorBlock, status) == 460,
          "char: the block: magic 0x4C445332 ('LDS2'), 720 bytes, the status at 460 (static_assert: every field's offset)");

    // what ReadHelper takes: the magic and the helper's clock (under 5000 ms), never `size`
    SensorClient s;
    Fresh(s);
    g_settings = Settings{};
    g_settings.sensorSource = 2;
    fake::helperUp = true;
    fake::now += kStep;
    s.Tick(fake::now);
    LDSensorBlock* b = fake::helper;
    auto write = [&](uint32_t magic, uint32_t size, ULONGLONG ageMs, float cpu, bool odd = false) {
        fake::now += kStep;
        b->seq += odd ? 1 : 2;
        b->magic = magic; b->size = size; b->flags = LD_HAS_CPU_TEMP; b->cpuTemp = cpu;
        b->tick = FakeTick() - ageMs;
        s.Tick(fake::now);
        return s.helperBlk.cpuTemp == cpu && s.lastSeq == b->seq;
    };
    Check(s.view && write(LD_SENSOR_MAGIC, sizeof(LDSensorBlock), 0, 41.f), "char: the helper's block with its magic and size: taken");
    Check(write(LD_SENSOR_MAGIC, 0, 0, 42.f), "char: ...size 0: taken all the same (the dock does not look at size)");
    Check(write(LD_SENSOR_MAGIC, 12345, 0, 43.f), "char: ...size 12345: taken all the same");
    Check(!write(0x4C445331u, sizeof(LDSensorBlock), 0, 44.f) && s.helperBlk.cpuTemp == 43.f, "char: ...another magic ('LDS1'): not taken");
    Check(write(LD_SENSOR_MAGIC, sizeof(LDSensorBlock), 4999, 45.f), "char: ...stamped 4999 ms ago by the helper's clock: taken");
    fake::now += kStep;
    b->cpuTemp = 45.5f; b->tick = FakeTick();   // changed without a new sequence number
    s.Tick(fake::now);
    Check(s.helperBlk.cpuTemp == 45.f, "char: ...the sequence number of the last one taken: not taken again");
    Check(!write(LD_SENSOR_MAGIC, sizeof(LDSensorBlock), 5000, 46.f) && s.helperBlk.cpuTemp == 45.f, "char: ...5000 ms ago: not taken");
    Check(!write(LD_SENSOR_MAGIC, sizeof(LDSensorBlock), 0, 47.f, true) && s.helperBlk.cpuTemp == 45.f, "char: ...while it writes (seq odd): not taken");
    b->seq += 1;
    Check(write(LD_SENSOR_MAGIC, sizeof(LDSensorBlock), 0, 48.f) && s.helperLive, "char: ...written out (seq even, new): taken");
    // Items(): sensorSource clamped to 0..2 (Load clamps it too: only a value set in the program gets here)
    g_settings.sensorSource = 5;
    const int high = s.Items().source;
    g_settings.sensorSource = -1;
    const int low = s.Items().source;
    g_settings.sensorSource = 2;
    Check(high == 2 && low == 0, "char: Items(): sensorSource 5 reads as 2 (PawnIO), -1 as 0 (off)");
    fake::helperUp = false;
    Fresh(s);
    g_settings = Settings{};
}

// The helper's install and start (admin_helper_process.cpp: HelperPath, RunHidden, InstallHelper, StartHelper, HelperTaskRegistered),
// the calls in order. Nothing real: no runas, no schtasks.exe.
void HelperProcess() {
    const std::wstring dir = L"C:\\Users\\dock\\AppData\\Local\\Programs\\LiquidDock\\";
    const std::wstring helper = dir + L"LiquidDockSensors.exe";
    const std::wstring schtasks = L"\"C:\\Windows\\System32\\schtasks.exe\"";
    fake::tracing = true;
    fake::logs.clear();

    // the install: one ShellExecuteEx "runas"
    fake::trace.clear();
    const int installs = fake::installs;
    Check(adminhelper::InstallHelper((HWND)(uintptr_t)0x1234) && fake::installs == installs + 1, "char: InstallHelper: started, true");
    Check(fake::seFile == helper, "char: ...the file: LiquidDockSensors.exe in the dock exe's folder (the whole path)");
    Check(fake::seVerb == L"runas" && fake::seParams == L"--install" && fake::seShow == SW_HIDE && fake::seMask == SEE_MASK_NOCLOSEPROCESS &&
          fake::seOwner == (HWND)(uintptr_t)0x1234 && fake::seSize == sizeof(SHELLEXECUTEINFOW),
          "char: ...runas --install, SW_HIDE, SEE_MASK_NOCLOSEPROCESS, owned by the window given");
    Check(fake::moduleSize == MAX_PATH, "char: ...the exe's path read into MAX_PATH characters");
    CheckTrace(L"GetModuleFileNameW | ShellExecuteExW runas " + helper + L" --install", "char: ...the calls: the exe's path, ShellExecuteEx, nothing waited for");
    fake::trace.clear();
    fake::seProcess = true;
    Check(adminhelper::InstallHelper(nullptr) && fake::seOwner == nullptr, "char: InstallHelper with no owner: started");
    CheckTrace(L"GetModuleFileNameW | ShellExecuteExW runas " + helper + L" --install | CloseHandle install process",
               "char: ...a process handle given back: closed at once, not waited on");
    fake::seProcess = false;
    // declined
    fake::trace.clear();
    fake::uacDeclined = true;
    Check(!adminhelper::InstallHelper(nullptr) && fake::installs == installs + 2, "char: InstallHelper, the UAC prompt declined: false");
    CheckTrace(L"GetModuleFileNameW | ShellExecuteExW runas " + helper + L" --install -> declined | Log sensor helper install not started (1223)",
               "char: ...logged 'sensor helper install not started (1223)' (ERROR_CANCELLED)");
    fake::uacDeclined = false;
    // the exe's path cut short (over MAX_PATH): up to the last backslash that is left; no backslash at all: the bare name
    fake::modulePath = L"C:\\" + std::wstring(200, L'd') + L"\\" + std::wstring(100, L'e') + L"\\LiquidDock.exe";
    adminhelper::InstallHelper(nullptr);
    Check(fake::seFile == L"C:\\" + std::wstring(200, L'd') + L"\\LiquidDockSensors.exe",
          "char: HelperPath with the exe's path cut at 259 characters: the folder up to the last backslash left in it");
    fake::modulePath = L"LiquidDock.exe";
    adminhelper::InstallHelper(nullptr);
    Check(fake::seFile == L"LiquidDockSensors.exe", "char: HelperPath with no backslash in the exe's path: the bare name LiquidDockSensors.exe");
    // StartHelper looks for the helper's file by that same path (not admin_helper's ShippedPath rule)
    fake::helperFile = false;
    fake::trace.clear();
    adminhelper::StartHelper();
    CheckTrace(L"GetModuleFileNameW | GetFileAttributesW LiquidDockSensors.exe",
               "char: StartHelper with no backslash in the exe's path: asks for the bare name LiquidDockSensors.exe");
    fake::modulePath = L"C:\\" + std::wstring(200, L'd') + L"\\" + std::wstring(100, L'e') + L"\\LiquidDock.exe";
    fake::trace.clear();
    adminhelper::StartHelper();
    CheckTrace(L"GetModuleFileNameW | GetFileAttributesW C:\\" + std::wstring(200, L'd') + L"\\LiquidDockSensors.exe",
               "char: StartHelper with the exe's path cut at 259 characters: asks in the folder up to the last backslash left");
    fake::modulePath = dir + L"LiquidDock.exe";

    // StartHelper: only when the helper's file is next to the dock's exe
    fake::trace.clear();
    const int processes = fake::processes;
    fake::helperFile = false;
    adminhelper::StartHelper();
    CheckTrace(L"GetModuleFileNameW | GetFileAttributesW " + helper, "char: StartHelper, no helper file in the dock's folder: nothing run, nothing logged");
    Check(fake::processes == processes, "char: ...no process");
    fake::helperFile = true;
    fake::processOk = true;
    fake::waitResult = WAIT_OBJECT_0; fake::exitCodeOk = true; fake::exitCode = 0;
    fake::trace.clear();
    adminhelper::StartHelper();
    CheckTrace(L"GetModuleFileNameW | GetFileAttributesW " + helper + L" | CreateProcessW " + schtasks +
               L" /Run /TN LiquidDockSensors | WaitForSingleObject 8000 | GetExitCodeProcess -> 0 | CloseHandle process | CloseHandle thread | Log sensor task run rc=0",
               "char: StartHelper: schtasks /Run /TN LiquidDockSensors, waited up to 8000 ms, its exit code, both handles closed, 'sensor task run rc=0'");
    Check(fake::processArgs == L"app=null inherit=0 flags=0x08000000 env=null cwd=null cb=104 si.flags=0x1 show=0",
          "char: ...CreateProcess: no application name, nothing inherited, CREATE_NO_WINDOW, STARTF_USESHOWWINDOW SW_HIDE");
    fake::trace.clear();
    fake::waitResult = WAIT_TIMEOUT; fake::exitCode = STILL_ACTIVE;
    adminhelper::StartHelper();
    CheckTrace(L"GetModuleFileNameW | GetFileAttributesW " + helper + L" | CreateProcessW " + schtasks +
               L" /Run /TN LiquidDockSensors | WaitForSingleObject 8000 -> timeout | GetExitCodeProcess -> 259 | CloseHandle process | CloseHandle thread | Log sensor task run rc=259",
               "char: ...still running after 8 s: not ended, its code STILL_ACTIVE logged as rc=259");
    fake::trace.clear();
    fake::waitResult = WAIT_OBJECT_0; fake::exitCodeOk = false;
    adminhelper::StartHelper();
    Check(!fake::logs.empty() && fake::logs.back() == L"sensor task run rc=1", "char: ...no exit code to be had: rc=1");
    fake::exitCodeOk = true;
    fake::processOk = false;
    fake::trace.clear();
    adminhelper::StartHelper();
    CheckTrace(L"GetModuleFileNameW | GetFileAttributesW " + helper + L" | CreateProcessW " + schtasks + L" /Run /TN LiquidDockSensors -> fails | Log sensor task run rc=-1",
               "char: ...schtasks.exe not started: nothing waited on or closed, rc=-1");

    // HelperTaskRegistered: schtasks /Query, exit code 0 only; the helper file not looked at
    fake::processOk = true;
    fake::helperFile = false;
    fake::trace.clear();
    fake::exitCode = 0;
    Check(adminhelper::HelperTaskRegistered(), "char: HelperTaskRegistered: schtasks /Query answers 0: registered");
    CheckTrace(L"CreateProcessW " + schtasks + L" /Query /TN LiquidDockSensors | WaitForSingleObject 8000 | GetExitCodeProcess -> 0 | CloseHandle process | CloseHandle thread",
               "char: ...the calls: no exe path, no file look, nothing logged");
    fake::exitCode = 1;
    Check(!adminhelper::HelperTaskRegistered(), "char: ...answers 1 (no such task): not registered");
    fake::waitResult = WAIT_TIMEOUT; fake::exitCode = STILL_ACTIVE;
    Check(!adminhelper::HelperTaskRegistered(), "char: ...still running after 8 s (259): not registered");
    fake::waitResult = WAIT_OBJECT_0; fake::exitCode = 0;
    fake::processOk = false;
    Check(!adminhelper::HelperTaskRegistered(), "char: ...schtasks.exe not started (-1): not registered");
    Check(fake::terminates == 0, "char: the helper's install and start: no process ended (TerminateProcess never called, the timed-out schtasks.exe left running)");
    fake::tracing = false;
    fake::trace.clear();
}

// SensorClient::Start: the settings from before the source (sourceFromCpuTemp) settled once, then the sampler thread.
void StartSettles() {
    static SensorClient clients[8];   // Start's thread holds its client: never freed (the thread is parked)
    int used = 0;
    const wchar_t* before = L"sensors: settings from before the source, PawnIO switch on - ";
    struct Case { bool pawnio, hwinfoCpu; int source; const wchar_t* log; };
    const Case cases[] = {
        { true, false, 2, L"PawnIO (PawnIO installed, HWiNFO CPU temperature none)" },
        { true, true, 1, L"HWiNFO Gadget (PawnIO installed, HWiNFO CPU temperature reported)" },
        { false, false, 1, L"HWiNFO Gadget (PawnIO missing, HWiNFO CPU temperature none)" },
        { false, true, 1, L"HWiNFO Gadget (PawnIO missing, HWiNFO CPU temperature reported)" },
    };
    fake::tracing = true;
    for (const Case& c : cases) {
        SensorClient& s = clients[used++];
        g_settings = Settings{};
        g_settings.sensorSource = 2;            // as Load took it from cpuTemp=1
        g_settings.sourceFromCpuTemp = true;
        g_settings.hwinfoCpuLabel = "CPU Package";
        fake::pawnio = c.pawnio;
        fake::readerFound = c.hwinfoCpu;
        fake::trace.clear();
        fake::samplerLabel.clear();
        const int saves = fake::saves;
        fake::savedSource = -99;
        s.Start();
        const bool up = WaitForSingleObject(fake::samplerUp, 5000) == WAIT_OBJECT_0;
        char what[200];
        std::snprintf(what, sizeof(what), "char: Start, settings from before the source, PawnIO %s, HWiNFO CPU %s: source %d, the flag down, saved once",
                      c.pawnio ? "installed" : "missing", c.hwinfoCpu ? "reported" : "none", c.source);
        Check(g_settings.sensorSource == c.source && !g_settings.sourceFromCpuTemp && fake::saves == saves + 1, what);
        std::snprintf(what, sizeof(what), "char: ...what Save wrote: source %d, the settled one (PawnIO %s, HWiNFO CPU %s; the next start's Load keeps it)",
                      c.source, c.pawnio ? "installed" : "missing", c.hwinfoCpu ? "reported" : "none");
        Check(fake::savedSource == c.source, what);
        std::snprintf(what, sizeof(what), "char: ...the calls in order: probe, ReadCpu, PawnIO's service, the log line, Save, the wake event (PawnIO %s, HWiNFO %s)",
                      c.pawnio ? "installed" : "missing", c.hwinfoCpu ? "reported" : "none");
        CheckTrace(L"Reader(CPU Package) | ReadCpu | OpenSCManagerW | OpenServiceW PawnIO | Log " + std::wstring(before) + c.log + L" | Save | CreateEventW (unnamed) auto unset", what);
        Check(up && fake::samplerLabel == L"CPU Package" && s.readHWiNFO == (c.source == 1) && s.readDrives && s.wake,
              "char: ...the sampler thread up with the label; its first pass by the settled source; the wake event kept");
        if (s.wake) { CloseHandle(s.wake); s.wake = nullptr; }
    }
    // settled already: no probe, no log, no save
    {
        SensorClient& s = clients[used++];
        g_settings = Settings{};
        g_settings.sensorSource = 2;
        g_settings.showSsd = false;
        g_settings.hwinfoCpuLabel = "";
        fake::samplerLabel = L"(not made)";
        fake::trace.clear();
        const int saves = fake::saves;
        s.Start();
        const bool up = WaitForSingleObject(fake::samplerUp, 5000) == WAIT_OBJECT_0;
        Check(g_settings.sensorSource == 2 && fake::saves == saves, "char: Start with settings of the source: nothing changed, nothing saved");
        CheckTrace(L"CreateEventW (unnamed) auto unset", "char: ...no probe, no PawnIO look, no log: only the wake event");
        Check(up && fake::samplerLabel.empty() && !s.readHWiNFO && !s.readDrives, "char: ...the sampler up (no label); first pass: no HWiNFO (PawnIO), no drives (hidden)");
        if (s.wake) { CloseHandle(s.wake); s.wake = nullptr; }
    }
    // no wake event to be had
    {
        SensorClient& s = clients[used++];
        g_settings = Settings{};
        fake::eventFails = true;
        fake::trace.clear();
        s.Start();
        const bool up = WaitForSingleObject(fake::samplerUp, 5000) == WAIT_OBJECT_0;
        fake::eventFails = false;
        CheckTrace(L"CreateEventW (unnamed) auto unset | Log sensors: no wake event (8): an opening panel waits up to 2 s for its first sample",
                   "char: Start with no wake event: logged with its error, the sampler started all the same");
        Check(up && !s.wake && s.readHWiNFO && s.readDrives, "char: ...the sampler up; first pass: HWiNFO (the default source), the drives");
    }
    fake::tracing = false;
    fake::trace.clear();
    fake::pawnio = true;
    g_settings = Settings{};
}

// SensorClient::Start's sampler loop, pass by pass: the pass object (FakeSensorThread) runs the
// passes asked for and waits for the test after each - what the loop asks it, with what, and what the Tick takes
std::wstring SamplerTrace() {
    std::lock_guard<std::mutex> lk(fake::samplerMu);
    std::wstring s;
    for (const auto& t : fake::samplerTrace) s += (s.empty() ? L"" : L" | ") + t;
    fake::samplerTrace.clear();
    return s;
}
void CheckSampler(const std::wstring& got, const std::wstring& want, const char* what) {
    if (!Check(got == want, what)) std::printf("  sampler: %ls\n  want:    %ls\n", got.c_str(), want.c_str());
}
void SamplerLoop() {
    static SensorClient loop, noWake, noThread;   // their threads hold them: never freed (parked)
    fake::samplerPassDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fake::samplerGoOn = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SamplerTrace();

    // two passes: the cell's sample alone, then (the settings changed, a panel opened) one with the panel's data
    g_settings = Settings{};
    g_settings.showSsd = false;          // HWiNFO (1), the drives hidden
    fake::samplerPasses = 2;
    fake::panelOnPass = 2;
    fake::now = 7000;                    // the helper's clock (GetTickCount64): 7,000,000 ms
    loop.Start();
    const bool up = WaitForSingleObject(fake::samplerUp, 5000) == WAIT_OBJECT_0;
    const bool first = WaitForSingleObject(fake::samplerPassDone, 5000) == WAIT_OBJECT_0;
    CheckSampler(SamplerTrace(),
                 L"Init | WaitMs open=0 | wait wake 2 | Want hwinfo=1 drives=0 | SampleCell open=0 tick=7000000 | SamplePanel open=0 tick=7000000 | WaitMs open=0",
                 "char: Start's sampler, its first pass: WaitMs (panel closed), the wait on the wake event for that long, Want(HWiNFO, drives) as the settings say, SampleCell, SamplePanel");
    Check(up && first && loop.localNew && loop.localBlk.cpuLoad == 11.f && !loop.localPanelNew,
          "char: ...the cell's sample handed over (localNew set), no panel data (SamplePanel false)");
    loop.Tick(fake::now);
    Check(loop.Live() && loop.version == 1 && loop.d.cpuLoad == 11.f && !loop.Panel().sampled && !loop.localNew, "char: ...taken on the next Tick");
    Check(fake::powerWant.view == 0 && !fake::powerWant.cpu && !fake::powerWant.gpu && !loop.localPowerNew && loop.powerVersion == 0,
          "power: the default settings (없음) - WantPower(0, off, off) before the pass, nothing published");
    g_settings.sensorSource = 2; g_settings.showSsd = true;
    g_settings.powerView = 1;            // 그래프, both switches on: with PawnIO the CPU's is not shown (HWiNFO only)
    fake::powerOnPass = 2;
    fake::now += 0.25;
    loop.Tick(fake::now);                // the sampler's view: no HWiNFO (PawnIO), the drives
    loop.SetPanelOpen(true, fake::now);  // the wake event set
    SetEvent(fake::samplerGoOn);
    const bool second = WaitForSingleObject(fake::samplerPassDone, 5000) == WAIT_OBJECT_0;
    CheckSampler(SamplerTrace(),
                 L"wait wake 2 | Want hwinfo=0 drives=1 | SampleCell open=1 tick=7000250 | SamplePanel open=1 tick=7000250 | WaitMs open=1",
                 "char: ...the second pass: the panel open (read after the wait), Want by the Tick's new view, the next WaitMs asked with the panel open");
    Check(second && loop.localNew && loop.localPanelNew && loop.localPanel.modules == 7 && loop.localBlk.cpuLoad == 12.f,
          "char: ...its cell sample and its panel data handed over");
    fake::now += 0.25;
    loop.Tick(fake::now);
    Check(loop.version == 2 && loop.d.cpuLoad == 12.f && loop.Panel().sampled && loop.Panel().modules == 7 && loop.Panel().drivesWithTemp == 2,
          "char: ...both taken on the next Tick");
    Check(fake::powerWant.view == 1 && !fake::powerWant.cpu && fake::powerWant.gpu,
          "power: 그래프 with the source PawnIO - WantPower(1, CPU off (HWiNFO only), GPU on), as the Tick before the pass said");
    Check(loop.Power().view == 1 && loop.Power().atMs == 7000250 && loop.Power().gpu.read && loop.Power().gpu.watts == 287.f &&
          loop.Power().gpu.limitW == 360.f && !loop.Power().cpu.read && loop.powerVersion == 1 && !loop.localPowerNew,
          "power: ...the pass's power handed over with the cell's sample and taken on the same Tick (powerVersion 1)");
    fake::powerOnPass = -1;
    g_settings.powerView = 0;

    // no wake event: the sampler sleeps its period
    g_settings = Settings{};
    fake::samplerPasses = 1;
    fake::panelOnPass = 0;
    fake::eventFails = true;
    noWake.Start();
    fake::eventFails = false;
    const bool up2 = WaitForSingleObject(fake::samplerUp, 5000) == WAIT_OBJECT_0;
    const bool done = WaitForSingleObject(fake::samplerPassDone, 5000) == WAIT_OBJECT_0;
    CheckSampler(SamplerTrace(),
                 L"Init | WaitMs open=0 | sleep 2 | Want hwinfo=1 drives=1 | SampleCell open=0 tick=7000500 | SamplePanel open=0 tick=7000500 | WaitMs open=0",
                 "char: Start with no wake event: the sampler sleeps WaitMs's period (Sleep), then the same pass");
    Check(up2 && done && !noWake.wake && noWake.localNew, "char: ...its sample handed over");

    // no thread to be had (std::thread throws): logged, nothing else
    fake::threadFails = true;
    fake::samplerPasses = 0;
    const size_t logs = fake::logs.size();
    noThread.Start();
    fake::threadFails = false;
    Check(fake::logs.size() == logs + 1 && fake::logs.back() == L"sensors: no thread to sample on: no readings this run" && !noThread.sampler.joinable(),
          "char: Start with no thread to be had: 'sensors: no thread to sample on: no readings this run', no sampler");
    if (noThread.wake) { CloseHandle(noThread.wake); noThread.wake = nullptr; }
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
    PowerDrops();
    Cell();
    Sources();
    fake::mainThread = GetCurrentThreadId();
    fake::samplerUp = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Layout();
    HelperProcess();
    StartSettles();
    SamplerLoop();
    return Summary("sensors_client_test");
}
