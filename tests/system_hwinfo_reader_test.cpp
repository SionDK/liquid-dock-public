// The HWiNFO Gadget reader (hwinfo::Reader in src/hwinfo.cpp) against a private registry hive: the
// dock's real code reads a "HKCU\Software\HWiNFO64\VSB" that lives in a hive file of this test
// (RegLoadAppKey: no other program sees it, HKCU is not written). The clock, the tick count and the
// HWiNFO process check (its process list and snapshot) are the test's. What it shows: no key and an
// empty key report nothing; a laptop's Korean HWiNFO reading is picked; between full scans only the
// picked reading is read (no enumeration); a better reading ticked later is taken at the next scan, a
// picked reading that goes away at once; values a closed HWiNFO left behind are not shown - also after the
// clock was set back - while a quiet key of a running HWiNFO is (it writes a value only when it changes);
// the process check made while the key is quiet reads the process list once every 5 s (a Toolhelp snapshot
// only when there is no list) until it finds HWiNFO, then holds its process and asks it with a zero wait,
// and the list agrees with the snapshot on this PC; memory temperatures are read with the CPU's by cached
// index, one full scan serving both, and missing ones do not pull the scans to 3 s; the GPU's memory
// readings (VRAM: its junction temperature, the amount in use) likewise; hundreds of readings
// with gaps, non-text and oversized values are read; and the reader returns when the key is deleted while
// it reads. HWiNFO's process is an event of the test's own (OpenProcess faked). The hive file sits next to
// the exe.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\system_hwinfo_reader_test.cpp /link advapi32.lib
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

HKEY g_hive = nullptr;                    // the root of the test's hive
bool g_keyThere = true;                   // HKCU\Software\HWiNFO64\VSB exists
bool g_hwinfoRuns = false;                // what the process list says
LONGLONG g_clockShift = 0;                // added to the system time (100 ns units)
ULONGLONG g_tick = 1000000;               // GetTickCount64
bool g_deleteAfterInfo = false;           // HWiNFO deletes the key right after the reader asked about it
std::atomic<long long> g_enumCalls{ 0 };  // RegEnumValueW calls (full scans)
std::atomic<bool> g_park{ false };        // a runaway enumeration stops burning a core once seen

LSTATUS WINAPI FakeRegOpenKeyExW(HKEY root, LPCWSTR sub, DWORD, REGSAM sam, PHKEY out) {
    if (root == HKEY_CURRENT_USER && sub && wcscmp(sub, L"Software\\HWiNFO64\\VSB") == 0 && g_keyThere)
        return RegOpenKeyExW(g_hive, L"VSB", 0, sam, out);
    return ERROR_FILE_NOT_FOUND;   // HKLM: nothing either
}
LSTATUS WINAPI FakeRegQueryInfoKeyW(HKEY k, LPWSTR a, LPDWORD b, LPDWORD c, LPDWORD d, LPDWORD e, LPDWORD f, LPDWORD g,
                                    LPDWORD h, LPDWORD i, LPDWORD j, PFILETIME written) {
    LSTATUS rc = RegQueryInfoKeyW(k, a, b, c, d, e, f, g, h, i, j, written);
    if (g_deleteAfterInfo) { g_deleteAfterInfo = false; RegDeleteKeyW(g_hive, L"VSB"); }
    return rc;
}
LSTATUS WINAPI FakeRegEnumValueW(HKEY k, DWORD i, LPWSTR name, LPDWORD cch, LPDWORD r, LPDWORD type, LPBYTE data, LPDWORD cb) {
    if (++g_enumCalls > 1000000 && g_park) Sleep(INFINITE);
    return RegEnumValueW(k, i, name, cch, r, type, data, cb);
}
void WINAPI FakeSystemTime(LPFILETIME ft) {
    GetSystemTimeAsFileTime(ft);
    ULARGE_INTEGER u; u.LowPart = ft->dwLowDateTime; u.HighPart = ft->dwHighDateTime;
    u.QuadPart += g_clockShift;
    ft->dwLowDateTime = u.LowPart; ft->dwHighDateTime = u.HighPart;
}
ULONGLONG WINAPI FakeTick64() { return g_tick; }

// The process check: NtQuerySystemInformation's process list, else a Toolhelp snapshot. Both list the
// same names: g_processes, or (when it is empty) the Idle process, System and HWiNFO64.EXE or explorer.exe.
std::vector<std::wstring> g_processes;
bool g_listFails = false;                 // NtQuerySystemInformation answers with an error: no list
ULONG g_listExtra = 0;                    // bytes the list takes beyond its names (threads, say)
ULONG g_listGrows = 0;                    // ...and grows by at every call (processes starting)
int g_lists = 0, g_snapshots = 0;         // lists asked for, snapshots made
bool g_realProcesses = false;             // this PC's own list and snapshot (the timing at the end)
size_t g_nextName = 0;

// the session the listed processes run in: this one's unless a test says another (fast user switching)
DWORD g_listSession = 0xFFFFFFFFu;        // 0xFFFFFFFF: this process's own
DWORD OwnSessionForTest() { DWORD s = 0; ProcessIdToSessionId(GetCurrentProcessId(), &s); return s; }
DWORD ListSession() { return g_listSession == 0xFFFFFFFFu ? OwnSessionForTest() : g_listSession; }
BOOL WINAPI FakeProcessIdToSessionId(DWORD pid, DWORD* session) {
    if (pid == GetCurrentProcessId()) return ProcessIdToSessionId(pid, session);
    *session = ListSession();
    return TRUE;
}

std::vector<std::wstring> Names() {
    if (!g_processes.empty()) return g_processes;
    return { L"", L"System", g_hwinfoRuns ? L"HWiNFO64.EXE" : L"explorer.exe" };
}
// an entry: its head with the name right after it, at 8-byte steps
ULONG EntryBytes(const std::wstring& name) {
    return ULONG((sizeof(SYSTEM_PROCESS_INFORMATION) + (name.size() + 1) * sizeof(wchar_t) + 7) & ~size_t(7));
}
using Query = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
NTSTATUS NTAPI FakeQuery(SYSTEM_INFORMATION_CLASS what, PVOID buf, ULONG bytes, PULONG need) {
    ++g_lists;
    if (g_listFails) return (NTSTATUS)0xC0000022L;   // STATUS_ACCESS_DENIED
    if (g_realProcesses) {
        static const auto real = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
        return real(what, buf, bytes, need);
    }
    const std::vector<std::wstring> names = Names();
    g_listExtra += g_listGrows;
    ULONG total = g_listExtra;
    for (auto& n : names) total += EntryBytes(n);
    if (need) *need = total;
    if (bytes < total) return (NTSTATUS)0xC0000004L;   // STATUS_INFO_LENGTH_MISMATCH
    BYTE* p = static_cast<BYTE*>(buf);
    for (size_t i = 0; i < names.size(); ++i) {
        const ULONG size = EntryBytes(names[i]) + (i ? 0 : g_listExtra);   // the extra bytes in the first entry
        auto* e = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(p);
        ZeroMemory(e, sizeof(*e));
        wchar_t* name = reinterpret_cast<wchar_t*>(p + sizeof(*e));
        wcscpy_s(name, names[i].size() + 1, names[i].c_str());
        e->ImageName.Length = USHORT(names[i].size() * sizeof(wchar_t));
        e->ImageName.MaximumLength = USHORT(e->ImageName.Length + sizeof(wchar_t));
        e->ImageName.Buffer = names[i].empty() ? nullptr : name;   // the Idle process has no name
        e->UniqueProcessId = (HANDLE)(ULONG_PTR)(i ? 1000 + 4 * i : 0);
        e->SessionId = ListSession();
        e->NextEntryOffset = i + 1 < names.size() ? size : 0;
        p += size;
    }
    return 0;
}
FARPROC WINAPI FakeGetProcAddress(HMODULE m, LPCSTR name) {
    if (name && strcmp(name, "NtQuerySystemInformation") == 0) return reinterpret_cast<FARPROC>(&FakeQuery);
    return GetProcAddress(m, name);
}
HANDLE WINAPI FakeSnapshot(DWORD flags, DWORD pid) {
    ++g_snapshots;
    if (g_realProcesses) return CreateToolhelp32Snapshot(flags, pid);
    return CreateEventW(nullptr, TRUE, FALSE, nullptr);   // closed by the reader
}
BOOL WINAPI FakeNext(HANDLE snap, LPPROCESSENTRY32W pe) {
    if (g_realProcesses) return Process32NextW(snap, pe);
    const std::vector<std::wstring> names = Names();
    if (g_nextName >= names.size()) return FALSE;
    pe->th32ProcessID = DWORD(g_nextName ? 1000 + 4 * g_nextName : 0);
    wcscpy_s(pe->szExeFile, names[g_nextName++].c_str());
    return TRUE;
}
BOOL WINAPI FakeFirst(HANDLE snap, LPPROCESSENTRY32W pe) {
    if (g_realProcesses) return Process32FirstW(snap, pe);
    g_nextName = 0;
    return FakeNext(snap, pe);
}

// HWiNFO's process, held by the reader once the list found it: an event of the test's own stands for it
// (set = the process exited). Not openable by default (another user's HWiNFO, say): the list every 5 s.
bool g_openable = false;
int g_opens = 0, g_waits = 0;             // OpenProcess calls, zero waits on the held handle
HANDLE g_process = nullptr;               // the test's side of the last handle given out
HANDLE WINAPI FakeOpenProcess(DWORD access, BOOL, DWORD pid) {
    ++g_opens;
    if (!g_openable || !pid || !(access & SYNCHRONIZE)) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    if (g_process) CloseHandle(g_process);
    g_process = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), g_process, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return dup;
}
DWORD WINAPI FakeWait(HANDLE h, DWORD ms) { ++g_waits; return WaitForSingleObject(h, ms); }
// the image of the process opened: HWiNFO's, unless a test has its id taken by another one meanwhile
const wchar_t* const kHWiNFOImage = L"C:\\Program Files\\HWiNFO64\\HWiNFO64.EXE";
std::wstring g_image = kHWiNFOImage;
BOOL WINAPI FakeImageName(HANDLE, DWORD, LPWSTR out, PDWORD n) {
    if (g_image.empty() || g_image.size() + 1 > *n) return FALSE;
    wcscpy_s(out, *n, g_image.c_str());
    *n = (DWORD)g_image.size();
    return TRUE;
}

}   // namespace

#define OpenProcess FakeOpenProcess
#define WaitForSingleObject FakeWait
#define RegOpenKeyExW FakeRegOpenKeyExW
#define RegQueryInfoKeyW FakeRegQueryInfoKeyW
#define RegEnumValueW FakeRegEnumValueW
#define GetSystemTimeAsFileTime FakeSystemTime
#define GetTickCount64 FakeTick64
#define CreateToolhelp32Snapshot FakeSnapshot
#define Process32FirstW FakeFirst
#define Process32NextW FakeNext
#define GetProcAddress FakeGetProcAddress
#define ProcessIdToSessionId FakeProcessIdToSessionId
#define QueryFullProcessImageNameW FakeImageName
#include "../src/hwinfo.cpp"
#undef ProcessIdToSessionId
#undef QueryFullProcessImageNameW
#undef OpenProcess
#undef WaitForSingleObject
#undef RegOpenKeyExW
#undef RegQueryInfoKeyW
#undef RegEnumValueW
#undef GetSystemTimeAsFileTime
#undef GetTickCount64
#undef GetProcAddress

#include "portable_check.h"

namespace {

HKEY Vsb() {
    HKEY k = nullptr;
    RegCreateKeyExW(g_hive, L"VSB", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr);
    return k;
}
void SetText(HKEY k, const std::wstring& name, const std::wstring& text) {
    RegSetValueExW(k, name.c_str(), 0, REG_SZ, (const BYTE*)text.c_str(), DWORD((text.size() + 1) * sizeof(wchar_t)));
}
// one reading as HWiNFO writes it (Color too: the reader leaves it alone)
void Put(int n, const wchar_t* sensor, const wchar_t* label, const wchar_t* value, const wchar_t* raw) {
    HKEY k = Vsb();
    const std::wstring i = std::to_wstring(n);
    SetText(k, L"Sensor" + i, sensor); SetText(k, L"Label" + i, label);
    SetText(k, L"Value" + i, value); SetText(k, L"ValueRaw" + i, raw); SetText(k, L"Color" + i, L"ff0000");
    RegCloseKey(k);
}
void Drop(int n) {
    HKEY k = Vsb();
    const std::wstring i = std::to_wstring(n);
    for (const wchar_t* f : { L"Sensor", L"Label", L"Value", L"ValueRaw", L"Color" }) RegDeleteValueW(k, (f + i).c_str());
    RegCloseKey(k);
}
void Clear() { RegDeleteKeyW(g_hive, L"VSB"); RegCloseKey(Vsb()); }

bool Near(float a, float b) { return std::fabs(a - b) < 0.05f; }
constexpr LONGLONG kSecond = 10000000;

// ReadCpu on a thread of its own: true when it came back within `ms` (r filled), false when it did not
bool ReadWithin(hwinfo::Reader& reader, hwinfo::CpuReading& r, DWORD ms) {
    struct Job { hwinfo::Reader* reader; hwinfo::CpuReading r; } job{ &reader, {} };
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* j = static_cast<Job*>(p);
        j->r = j->reader->ReadCpu();
        return 0;
    }, &job, 0, nullptr);
    const bool back = WaitForSingleObject(t, ms) == WAIT_OBJECT_0;
    if (back) r = job.r;
    else g_park = true;   // left parked (Sleep) inside the enumeration; the process ends it
    CloseHandle(t);
    return back;
}

}   // namespace

int main() {
    // the private hive, made new for every run, next to this test's exe (build\tests\)
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring hive = exe;
    hive = hive.substr(0, hive.find_last_of(L'\\') + 1) + L"system_hwinfo_reader_test.hiv";
    for (const wchar_t* ext : { L"", L".LOG1", L".LOG2" }) DeleteFileW((hive + ext).c_str());
    LSTATUS loaded = RegLoadAppKeyW(hive.c_str(), &g_hive, KEY_ALL_ACCESS, 0, 0);
    if (!Check(loaded == ERROR_SUCCESS, "a private hive for the HWiNFO key (RegLoadAppKey)")) return Summary("system_hwinfo_reader_test");

    // ---- nothing reported
    {
        g_keyThere = false;
        hwinfo::Reader reader;
        hwinfo::CpuReading r = reader.ReadCpu();
        Check(!r.running && !r.found, "no Gadget key (HWiNFO never reported on this PC): nothing");
        g_keyThere = true;
        Clear();
        r = reader.ReadCpu();
        Check(!r.running && !r.found, "an empty key (reporting on, nothing ticked): HWiNFO is not taken as running");
    }

    // ---- a laptop's reading (HWiNFO in Korean, 2026-10-28: Sensor0..Color0 as read from a test PC)
    {
        Clear();
        Put(0, L"CPU [#0]: Intel Core Ultra 7 155H: DTS", L"코어 온도", L"42 °C", L"42");
        hwinfo::Reader reader;
        hwinfo::CpuReading r = reader.ReadCpu();
        Check(r.running && r.found && Near(r.celsius, 42), "a laptop's Korean reading (코어 온도, 42 °C) is the CPU temperature");

        Put(0, L"CPU [#0]: Intel Core Ultra 7 155H: DTS", L"코어 온도", L"43 °C", L"43");
        g_tick += 1000; g_enumCalls = 0;
        r = reader.ReadCpu();
        Check(r.found && Near(r.celsius, 43) && g_enumCalls == 0, "a second later: the picked reading is read by name, nothing enumerated");

        Put(1, L"CPU [#0]: Intel Core Ultra 7 155H: DTS", L"CPU Package", L"47 °C", L"47");   // ticked later
        g_tick += 1000;
        r = reader.ReadCpu();
        Check(r.found && Near(r.celsius, 43), "a better reading ticked later is not looked for between scans");
        g_tick += 10000; g_enumCalls = 0;
        r = reader.ReadCpu();
        Check(r.found && Near(r.celsius, 47) && g_enumCalls > 0, "...it is taken at the next full scan (10 s)");

        Drop(1);   // unticked: the picked reading goes away
        g_tick += 1000;
        r = reader.ReadCpu();
        Check(r.found && Near(r.celsius, 43), "the picked reading goes away: the next best is taken at once");

        Drop(0);
        Put(2, L"GPU [#0]: Intel Arc 140V GPU", L"GPU 온도", L"40 °C", L"40");
        g_tick += 1000;
        r = reader.ReadCpu();
        Check(r.running && !r.found, "only a GPU temperature left: HWiNFO runs, no CPU temperature (never the GPU's)");
    }

    // ---- freshness
    {
        Clear();
        Put(0, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"52.4 °C", L"52.4");
        hwinfo::Reader reader;
        g_hwinfoRuns = true; g_clockShift = 90 * kSecond; g_tick += 6000;
        hwinfo::CpuReading r = reader.ReadCpu();
        // HWiNFO 8.54 writes a value only when it changes: a steady temperature leaves the key
        // quiet. A key quiet for 60 s used to drop the CPU temperature while HWiNFO ran.
        Check(r.running && r.found && Near(r.celsius, 52.4f), "written 90 s ago while HWiNFO runs (a steady temperature): still shown");
        g_hwinfoRuns = false; g_clockShift = 30 * kSecond; g_tick += 6000;
        r = reader.ReadCpu();
        Check(!r.running && !r.found, "written 30 s ago and HWiNFO closed: the values it left are not shown");
        g_hwinfoRuns = true; g_tick += 6000;
        r = reader.ReadCpu();
        Check(r.running && r.found && Near(r.celsius, 52.4f), "...quiet for 30 s while HWiNFO runs (a slow polling period): shown");

        // The clock went back after HWiNFO was closed (a time sync correcting a fast clock, a manual change):
        // the key's last write now lies in the future. Its age was taken as 0 - "fresh" - and the value a
        // closed HWiNFO left behind was shown as live until the clock caught up.
        g_hwinfoRuns = false; g_clockShift = -3600 * kSecond; g_tick += 6000;
        r = reader.ReadCpu();
        Check(!r.running && !r.found, "the clock went back an hour after HWiNFO closed: its last value is not shown as live for that hour");
        g_hwinfoRuns = true; g_tick += 6000;
        r = reader.ReadCpu();
        Check(r.running && r.found, "...while HWiNFO runs, a write time ahead of the clock still counts as written");
        g_tick += 120000;
        r = reader.ReadCpu();
        Check(r.running && r.found, "...and not written for 2 minutes since, HWiNFO still running (a steady temperature): still shown");
        // set back by less than the key has been quiet: written 2 min 30 s ago by the tick count, 10 s by the clock,
        // and HWiNFO closed meanwhile
        g_hwinfoRuns = false; g_clockShift = 10 * kSecond; g_tick += 6000;
        r = reader.ReadCpu();
        Check(!r.running && !r.found, "a clock set back by less than the key has been quiet (2 min 30 s, the clock says 10 s), HWiNFO closed: not shown");
        g_hwinfoRuns = true;
        g_clockShift = 0;
        // the registry stamps a write in the clock's coarse steps (15.6 ms): let real time pass, as it does
        // between two of HWiNFO's writes, so that this one gets a write time of its own
        Sleep(50);
        Put(0, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"53.1 °C", L"53.1");
        g_tick += 1000;
        r = reader.ReadCpu();
        Check(r.running && r.found && Near(r.celsius, 53.1f), "...HWiNFO's next write is shown at once");
    }

    // ---- the process check, asked while the key is quiet: once every 5 s, from NtQuerySystemInformation's
    // process list (a Toolhelp snapshot, the list it is made from copied into a section, cost 4.5 ms a check
    // with 290 processes on a test laptop: every 5 s for good once a closed HWiNFO left its values behind)
    {
        Clear();
        Put(0, L"CPU [#0]: Intel Core Ultra 7 155H: DTS", L"코어 온도", L"42 °C", L"42");
        hwinfo::Reader reader;
        g_hwinfoRuns = false; g_clockShift = 90 * kSecond; g_tick += 6000;
        g_lists = g_snapshots = 0;
        hwinfo::CpuReading r = reader.ReadCpu();
        Check(!r.running && !r.found && g_lists == 1 && g_snapshots == 0, "a quiet key, HWiNFO closed: one process list read, no snapshot");
        int hourLists = g_lists, hourReads = 1;
        for (; hourReads < 3600; ++hourReads) { g_tick += 1000; r = reader.ReadCpu(); }   // the sensor thread's second
        hourLists = g_lists;
        std::printf("  an hour of reads (1 s apart) with the key quiet: %d process lists, %d snapshots\n", hourLists, g_snapshots);
        Check(hourLists == 720 && g_snapshots == 0 && !r.running, "...an hour of it: a list every 5 s (720), as the snapshots were, and no snapshot");

        g_hwinfoRuns = true; g_lists = 0;
        int seenAfter = 0;
        for (; seenAfter < 10 && !r.running; ++seenAfter) { g_tick += 1000; r = reader.ReadCpu(); }
        Check(r.running && seenAfter <= 5 && g_lists == 1, "HWiNFO started (Gadget reporting off): seen at the next check, within 5 s");

        auto runs = [&](std::vector<std::wstring> names) {
            g_processes = std::move(names);
            g_tick += 5000;
            const bool running = reader.ReadCpu().running;
            g_processes.clear();
            return running;
        };
        Check(runs({ L"", L"System", L"svchost.exe", L"hwinfo32.exe" }) && runs({ L"HWiNFO64.EXE", L"explorer.exe" }),
              "HWiNFO32 or HWiNFO64, first or last in the list, in any case: running");
        Check(!runs({ L"", L"HWiNFO64" }) && !runs({ L"", L"myhwinfo64.exe" }) && !runs({ L"" }),
              "a name without .exe, one that only contains hwinfo, only the Idle process: not HWiNFO");

        // the list takes more than the reader's first buffer: asked again at the size Windows gives, which
        // the next check asks with at once
        g_listExtra = 200 * 1024; g_lists = g_snapshots = 0; g_tick += 5000;
        r = reader.ReadCpu();
        Check(r.running && g_lists == 2 && g_snapshots == 0, "a list larger than the first buffer: asked again at the size it needs");
        g_lists = 0; g_tick += 5000;
        r = reader.ReadCpu();
        Check(r.running && g_lists == 1, "...and the next check asks once, with that size");
        // processes starting faster than the list is read: three calls at most, then the snapshot answers
        g_listGrows = 1024 * 1024; g_lists = g_snapshots = 0; g_tick += 5000;
        r = reader.ReadCpu();
        Check(r.running && g_lists == 3 && g_snapshots == 1, "a list that outgrows every buffer: three calls, then the Toolhelp snapshot answers");
        g_listGrows = 0; g_listExtra = 0;
        // no list to be had at all: the snapshot, as before
        g_listFails = true; g_lists = g_snapshots = 0; g_tick += 5000;
        r = reader.ReadCpu();
        g_hwinfoRuns = false; g_tick += 5000;
        hwinfo::CpuReading closed = reader.ReadCpu();
        Check(r.running && !closed.running && g_lists == 2 && g_snapshots == 2, "no process list (the call fails): the Toolhelp snapshot answers, as before");
        g_listFails = false;

        // this PC: the dock's list and the snapshot say the same, and what each costs
        g_realProcesses = true;
        constexpr int kChecks = 20;
        LARGE_INTEGER f, t0, t1, t2; QueryPerformanceFrequency(&f);
        bool listSays = false, snapshotSays = false;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < kChecks; ++i) { g_tick += 5000; listSays = reader.ReadCpu().running; }
        QueryPerformanceCounter(&t1);
        g_listFails = true;
        for (int i = 0; i < kChecks; ++i) { g_tick += 5000; snapshotSays = reader.ReadCpu().running; }
        QueryPerformanceCounter(&t2);
        g_listFails = false; g_realProcesses = false;
        std::printf("  this PC: HWiNFO %s; a check from the process list %.2f ms, from a snapshot %.2f ms\n", listSays ? "runs" : "does not run",
                    (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / kChecks, (t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart / kChecks);
        Check(listSays == snapshotSays, "this PC's process list and its Toolhelp snapshot agree on whether HWiNFO runs");
        g_clockShift = 0;
    }

    // ---- HWiNFO's process held: found once by the list, then asked with a zero wait (the list every 5 s cost
    // 0.8 - 3.2 ms on a test desktop, and with HWiNFO writing only changed values the key is quiet for long)
    {
        Clear();
        Put(0, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"47.2 °C", L"47.2");
        hwinfo::Reader reader;
        g_hwinfoRuns = true; g_openable = true; g_clockShift = 90 * kSecond; g_tick += 6000;
        g_lists = g_snapshots = g_opens = g_waits = 0;
        hwinfo::CpuReading r = reader.ReadCpu();
        Check(r.running && r.found && g_lists == 1 && g_opens == 1, "a quiet key, HWiNFO running: the list read once, HWiNFO's process opened");
        bool allShown = true;
        for (int i = 1; i < 3600; ++i) { g_tick += 1000; r = reader.ReadCpu(); allShown &= r.found && Near(r.celsius, 47.2f); }
        std::printf("  an hour of reads, a steady 47.2 °C (key quiet), HWiNFO held: %d lists, %d opens, %d zero waits\n", g_lists, g_opens, g_waits);
        Check(allShown, "an hour of a steady temperature (the key never written): shown all along");
        Check(g_lists == 1 && g_snapshots == 0 && g_opens == 1 && g_waits == 3599, "...asked with a zero wait on the held process each time: no list after the first (was 720 an hour)");

        SetEvent(g_process); g_hwinfoRuns = false;   // HWiNFO exits
        g_tick += 1000; g_lists = 0;
        r = reader.ReadCpu();
        Check(!r.running && !r.found && g_lists == 1, "HWiNFO exits: seen on the next read, the list read at once (nothing there)");
        g_tick += 1000;
        r = reader.ReadCpu();
        Check(!r.running && g_lists == 1, "...then the list every 5 s, as before");
        g_hwinfoRuns = true; g_lists = 0; g_opens = 0;
        int seenAfter = 0;
        for (; seenAfter < 10 && !r.running; ++seenAfter) { g_tick += 1000; r = reader.ReadCpu(); }
        Check(r.running && r.found && seenAfter <= 5 && g_lists == 1 && g_opens == 1, "HWiNFO started again: found by the next list (within 5 s) and held again");

        // the id the list gave taken by another process before it was opened (HWiNFO exited just then): that
        // process is let go at once, not held for HWiNFO (the list every 5 s, as while HWiNFO cannot be opened)
        {
            Clear();
            Put(0, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"47.2 °C", L"47.2");
            hwinfo::Reader reused;
            g_image = L"C:\\Windows\\System32\\notepad.exe";
            g_tick += 6000; g_lists = g_opens = g_waits = 0;
            r = reused.ReadCpu();
            g_tick += 1000;
            r = reused.ReadCpu();
            Check(g_opens == 1 && g_waits == 0, "an id taken by another program between the list and the open: not held (no zero wait on it)");
            g_tick += 5000; g_lists = 0;
            r = reused.ReadCpu();
            Check(g_lists == 1 && g_opens == 2, "...the list read again 5 s on, as for an HWiNFO that cannot be opened");
            g_image = kHWiNFOImage;
        }
        // another user's HWiNFO (fast user switching: the process in their session, this user's key left by an
        // HWiNFO now closed): not counted, by the list or by the snapshot - the key's values go stale as before
        {
            Clear();
            Put(0, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"47.2 °C", L"47.2");
            hwinfo::Reader other;
            g_listSession = OwnSessionForTest() + 1;
            g_tick += 6000; g_lists = g_opens = 0;
            r = other.ReadCpu();
            Check(!r.running && !r.found && g_lists == 1 && g_opens == 0, "HWiNFO in another session, key quiet: not running here, its values not shown");
            g_listFails = true; g_tick += 5000;
            r = other.ReadCpu();
            Check(!r.running && !r.found, "...nor by the Toolhelp snapshot");
            g_listFails = false; g_listSession = 0xFFFFFFFFu; g_tick += 5000;
            r = other.ReadCpu();
            Check(r.running && r.found, "...HWiNFO in this session: running, shown");
        }

        // HWiNFO that cannot be opened (run as administrator): the list every 5 s, as before - and still shown
        Clear();
        Put(0, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"47.2 °C", L"47.2");
        hwinfo::Reader closedOff;
        g_openable = false; g_tick += 6000; g_lists = g_opens = 0;
        allShown = true;
        for (int i = 0; i < 3600; ++i) { g_tick += 1000; r = closedOff.ReadCpu(); allShown &= r.found; }
        Check(allShown && g_lists == 720 && g_opens == 720, "HWiNFO not openable, key quiet for an hour: shown, the list every 5 s (720) as before");
        g_clockShift = 0;
    }

    // ---- memory temperatures: read with the CPU's by cached index, one full scan shared
    {
        Clear();
        Put(0, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"48.6 °C", L"48.6");
        // as read on a test PC (HWiNFO in Korean), B2 first to show the slot order
        Put(1, L"DDR5 DIMM [#3]: A-DATA Technology AX5U6000C3032G-BLARWH (P0 CHANNEL B/DIMM 1)", L"SPD Hub 온도", L"36.0 °C", L"36.0");
        Put(2, L"DDR5 DIMM [#1]: A-DATA Technology AX5U6000C3032G-BLARWH (P0 CHANNEL A/DIMM 1)", L"SPD Hub 온도", L"35.8 °C", L"35.8");
        hwinfo::Reader reader;
        g_tick += 6000; g_enumCalls = 0;
        hwinfo::Reading r = reader.Read(true);
        Check(r.cpuFound && r.dimms.size() == 2 && g_enumCalls > 0, "CPU and two memory temperatures: found by one full scan");
        Check(r.dimms.size() == 2 && r.dimms[0].slot == 1 && r.dimms[0].entry == 2 && Near(r.dimms[0].celsius, 35.8f) &&
              r.dimms[0].locator == L"P0 CHANNEL A/DIMM 1" && r.dimms[1].slot == 3 && Near(r.dimms[1].celsius, 36.0f),
              "...in slot order with their (Bank/Device), the Gadget index as entry");
        Put(2, L"DDR5 DIMM [#1]: A-DATA Technology AX5U6000C3032G-BLARWH (P0 CHANNEL A/DIMM 1)", L"SPD Hub 온도", L"36.3 °C", L"36.3");
        g_tick += 1000; g_enumCalls = 0;
        r = reader.Read(true);
        Check(r.dimms.size() == 2 && Near(r.dimms[0].celsius, 36.3f) && g_enumCalls == 0, "a second later: read by their cached indices, nothing enumerated");
        g_tick += 1000;
        r = reader.Read(false);
        Check(r.cpuFound && r.dimms.empty(), "not asked for (panel closed): no memory temperatures");
        Drop(1);   // B2 unticked
        g_tick += 1000; g_enumCalls = 0;
        r = reader.Read(true);
        Check(r.dimms.size() == 1 && r.dimms[0].slot == 1 && g_enumCalls > 0, "one unticked: a full scan at once, the other stays");

        // no memory temperature ticked (most PCs): the scans stay 10 s apart - they are not pulled to 3 s
        Clear();
        Put(0, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"48.6 °C", L"48.6");
        hwinfo::Reader noDimms;
        int scans = 0;
        for (int i = 0; i < 60; ++i) {
            g_tick += 1000; g_enumCalls = 0;
            r = noDimms.Read(true);
            scans += g_enumCalls > 0;
        }
        Check(r.cpuFound && r.dimms.empty() && scans == 6, "a minute of panel reads without memory temperatures: 6 full scans (every 10 s, not every 3 s)");
        // memory temperatures but no CPU temperature: the 3 s rescans as before (the CPU's), the memory read meanwhile
        Clear();
        Put(5, L"DDR5 DIMM [#1] (P0 CHANNEL A/DIMM 1)", L"SPD Hub Temperature", L"35.0 °C", L"35.0");
        hwinfo::Reader noCpu;
        scans = 0;
        for (int i = 0; i < 30; ++i) {
            g_tick += 1000; g_enumCalls = 0;
            r = noCpu.Read(true);
            scans += g_enumCalls > 0;
        }
        Check(!r.cpuFound && r.dimms.size() == 1 && Near(r.dimms[0].celsius, 35.0f) && scans == 10,
              "memory only, no CPU temperature: shown every read, the full scan every 3 s for the CPU (10 in 30 s)");
    }

    // ---- the GPU's memory (VRAM): read with the others by cached index, the one full scan shared
    {
        // another test PC's key (HWiNFO in Korean)
        const wchar_t* aero = L"dGPU [#0]: NVIDIA GeForce RTX 4060 Ti: ASUS Dual GeForce RTX 4060 Ti OC 8G (DUAL-RTX4060TI-O8G)";
        Clear();
        Put(0, L"CPU [#0]: AMD Ryzen 7 7800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"38.1 °C", L"38.1");
        Put(1, L"DDR5 DIMM [#0]: Samsung", L"SPD Hub 온도", L"29.2 °C", L"29.2");
        Put(2, L"DDR5 DIMM [#2]: Team Group UD5-5200 (P0 CHANNEL B/DIMM 0)", L"SPD Hub 온도", L"29.2 °C", L"29.2");
        Put(3, aero, L"GPU 메모리 접합 온도", L"44.0 °C", L"44.0");
        hwinfo::Reader closed;
        g_tick += 6000; g_enumCalls = 0;
        hwinfo::Reading r = closed.Read(false);
        Check(r.cpuFound && r.gpus.empty() && r.dimms.empty() && g_enumCalls > 0, "a full scan with the panel closed: the CPU only, no memory or GPU memory readings handed over");
        g_tick += 1000; g_enumCalls = 0;
        r = closed.Read(true);
        Check(r.gpus.size() == 1 && r.dimms.size() == 2 && g_enumCalls == 0, "...the panel opens a second later: they are there at once, from that scan's picks (nothing enumerated)");
        hwinfo::Reader reader;
        g_tick += 6000; g_enumCalls = 0;
        r = reader.Read(true);
        Check(r.cpuFound && Near(r.cpuCelsius, 38.1f) && r.dimms.size() == 2 && g_enumCalls > 0, "that PC's key: the CPU and two memory temperatures by one full scan");
        Check(r.gpus.size() == 1 && r.gpus[0].gpu == 0 && r.gpus[0].sensor == aero && r.gpus[0].hasTemp && Near(r.gpus[0].celsius, 44.0f) &&
              !r.gpus[0].hasUsed && !r.gpus[0].hasUsage, "...and the GPU's memory junction temperature (\"GPU 메모리 접합 온도\", 44.0 °C) with its sensor's name");
        Put(3, aero, L"GPU 메모리 접합 온도", L"46.0 °C", L"46.0");
        g_tick += 1000; g_enumCalls = 0;
        r = reader.Read(true);
        Check(r.gpus.size() == 1 && Near(r.gpus[0].celsius, 46.0f) && r.dimms.size() == 2 && g_enumCalls == 0, "a second later: read by its cached index, nothing enumerated");
        g_tick += 1000;
        r = reader.Read(false);
        Check(r.cpuFound && r.gpus.empty() && r.dimms.empty(), "not asked for (panel closed): no GPU memory readings");
        // the amount in use ticked later: taken at the next full scan, with the temperature
        Put(4, aero, L"GPU Memory Allocated", L"1,905 MB", L"1905");
        g_tick += 1000; g_enumCalls = 0;
        r = reader.Read(true);
        Check(r.gpus.size() == 1 && !r.gpus[0].hasUsed && g_enumCalls == 0, "an amount ticked later: not looked for before the next full scan");
        Put(0, L"CPU [#0]: AMD Ryzen 7 7800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"38.4 °C", L"38.4");   // the key written meanwhile
        g_tick += 10000; g_enumCalls = 0;
        r = reader.Read(true);
        Check(r.gpus.size() == 1 && r.gpus[0].hasUsed && Near(r.gpus[0].usedMB, 1905.0f) && r.gpus[0].hasTemp && g_enumCalls > 0,
              "...taken at the next full scan (10 s): 1,905 MB with the temperature");
        Drop(3);   // the temperature unticked
        g_tick += 1000; g_enumCalls = 0;
        r = reader.Read(true);
        Check(r.gpus.size() == 1 && !r.gpus[0].hasTemp && r.gpus[0].hasUsed && g_enumCalls > 0, "the temperature unticked: a full scan at once, the amount stays");
        Drop(4);
        g_tick += 1000; g_enumCalls = 0;
        r = reader.Read(true);
        Check(r.gpus.empty() && r.cpuFound && r.dimms.size() == 2 && g_enumCalls > 0, "the amount unticked too: no GPU memory readings, the others as before");
        int scans = 0;
        for (int i = 0; i < 30; ++i) {
            Put(0, L"CPU [#0]: AMD Ryzen 7 7800X3D: Enhanced", L"CPU (Tctl/Tdie)", i % 2 ? L"38.4 °C" : L"38.6 °C", i % 2 ? L"38.4" : L"38.6");
            g_tick += 1000; g_enumCalls = 0;
            r = reader.Read(true);
            scans += g_enumCalls > 0;
        }
        Check(r.gpus.empty() && scans == 3, "30 s of panel reads without GPU memory readings: 3 full scans (every 10 s, not pulled closer)");
    }

    // ---- many readings, gaps, values that are not ours
    {
        Clear();
        for (int n = 0; n < 400; n += (n % 7 == 3 ? 2 : 1)) {   // gaps: HWiNFO drops the values of a sensor that stops reporting
            if (n == 377) Put(n, L"CPU [#0]: Intel Core i5-12600KF: DTS", L"CPU Package", L"61 °C", L"61");
            else if (n % 3 == 0) Put(n, L"GPU [#0]: NVIDIA GeForce RTX 4080 SUPER", L"GPU Temperature", L"44 °C", L"44");
            else Put(n, L"System: ASUS", L"Virtual Memory Committed", L"12,345 MB", L"12345");
        }
        HKEY k = Vsb();
        DWORD dw = 7; RegSetValueExW(k, L"Value5", 0, REG_DWORD, (const BYTE*)&dw, sizeof(dw));   // not text
        SetText(k, L"Label9", std::wstring(900, L'x'));                                          // longer than a read
        SetText(k, std::wstring(80, L'L') + L"1", L"CPU Package");                                // a name longer than ours
        RegCloseKey(k);
        hwinfo::Reader reader;
        g_tick += 6000;
        hwinfo::CpuReading r = reader.ReadCpu();
        Check(r.found && Near(r.celsius, 61), "400 readings with gaps, a number value, an oversized label: the CPU Package at 377 is found");
    }

    // ---- the key deleted while it is read
    {
        Clear();
        Put(0, L"CPU [#0]: Intel Core Ultra 7 155H: DTS", L"코어 온도", L"42 °C", L"42");
        hwinfo::Reader reader;
        hwinfo::CpuReading r = reader.ReadCpu();
        // a handle to a deleted key: RegEnumValueW answers ERROR_KEY_DELETED for every index, never ERROR_NO_MORE_ITEMS
        HKEY open = nullptr;
        RegOpenKeyExW(g_hive, L"VSB", 0, KEY_READ, &open);
        RegDeleteKeyW(g_hive, L"VSB");
        wchar_t name[64]; DWORD cch = 64, type = 0; BYTE data[64]; DWORD cb = sizeof(data);
        LSTATUS rc = RegEnumValueW(open, 0, name, &cch, nullptr, &type, data, &cb);
        std::printf("  RegEnumValueW on a deleted key: %ld\n", rc);
        RegCloseKey(open);
        Clear();
        Put(0, L"CPU [#0]: Intel Core Ultra 7 155H: DTS", L"코어 온도", L"42 °C", L"42");

        // HWiNFO deletes the key (Gadget reporting switched off, its sensor list rebuilt) between the reader's
        // RegQueryInfoKey and its read of the picked value: the fast path fails, and the full scan enumerated
        // the deleted key for ever on the sensor thread (a core at 100 %, the sensor panel frozen).
        g_tick += 1000; g_enumCalls = 0; g_deleteAfterInfo = true;
        const bool back = ReadWithin(reader, r, 3000);
        std::printf("  RegEnumValueW calls in that read: %lld%s\n", g_enumCalls.load(), back ? "" : " (still going after 3 s)");
        Check(back, "the key deleted while it is read: ReadCpu comes back (no endless enumeration of a deleted key)");
        Check(back && g_enumCalls <= 1, "...the scan ends at the first answer from the deleted key");
        Check(!back || !r.found, "...and reports no CPU temperature for that read");
        // HWiNFO makes the key again: the scan 3 s later (no CPU temperature meanwhile) opens the new one
        Put(0, L"CPU [#0]: Intel Core Ultra 7 155H: DTS", L"코어 온도", L"44 °C", L"44");
        g_tick += 3000;
        if (back) r = reader.ReadCpu();
        Check(back && r.running && r.found && Near(r.celsius, 44), "...and the key HWiNFO makes again is read at the next scan (3 s)");
    }

    const int failures = Summary("system_hwinfo_reader_test");
    std::fflush(stdout);
    ExitProcess(failures);   // a reader left parked in a runaway enumeration ends with the process
}
