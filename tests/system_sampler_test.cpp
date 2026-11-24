// The dock's own sensor sampler (hw::Sampler in src/hw_sample.h, sampled every 2 s - every second while a
// panel is open - by sensors.cpp's thread) and the panel's drive list and memory slots (src/hw_panel.h), with
// the Windows calls they make faked: the system times, the drive letters, volumes and disks (number, bus,
// name, temperature, extents), the SMBIOS table, NVML and PDH. What it shows: the CPU load from the times
// of two samples, the first sample measured from Init; the system drive's temperature asked every 5 s by
// the clock whether the thread wakes every 2 s or every second, the last answer standing in between (a
// temperature query every second keeps an NVMe drive out of its low-power states), and not
// asked while the panel's list reads it; an implausible, missing or short temperature answer is not shown;
// the panel's drives are the internal ones with a letter, one per disk, by letter, with cleaned names and
// their temperatures; the memory slots come from the SMBIOS table; the GPU's memory block (VRAM) takes its
// amounts from NVML and its temperature from NVML where the GPU reports one (asked only while a panel is
// open, never again after "not supported"), otherwise from HWiNFO, and without NVML the adapter's size
// and HWiNFO's readings. Touches nothing: no drive is opened, no driver library loaded.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\system_sampler_test.cpp /link advapi32.lib pdh.lib
#ifndef NOMINMAX
#define NOMINMAX   // as common.h has it before windows.h: hw_sample.h calls std::min
#endif
#include <windows.h>
#include <winioctl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

ULONGLONG g_idle = 0, g_kernel = 0, g_user = 0;   // what GetSystemTimes says (100 ns units, all processors)

// the disks, by number; disk 1 is the system disk (the Windows folder's volume, C:)
struct FakeDisk {
    STORAGE_BUS_TYPE bus = BusTypeNvme;
    bool removable = false;
    const char* vendor = "";
    const char* product = "Samsung SSD 990 PRO 1TB     ";   // space-padded, as drives report it
    bool tempAnswers = true;
    SHORT temp = 41;
    DWORD tempBytes = sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR);   // what the temperature answer returns
    int tempQueries = 0;
    int nameQueries = 0;   // StorageDeviceProperty asked
};
std::map<int, FakeDisk> g_disks;
// the drive letters
struct FakeVolume {
    UINT type = DRIVE_FIXED;
    int disk = 1;
    bool extentsOnly = false;   // a dynamic volume: no device number, its disk from its extents
};
std::map<wchar_t, FakeVolume> g_volumes = { { L'C', {} } };
std::map<HANDLE, std::wstring> g_paths;   // what each handle the code opened stands for
int g_opens = 0, g_volumeOpens = 0;      // handles opened; of them volumes (a drive letter's)
std::vector<BYTE> g_smbios;              // GetSystemFirmwareTable('RSMB') answer; empty: none

FILETIME Ft(ULONGLONG v) { FILETIME f; f.dwLowDateTime = (DWORD)v; f.dwHighDateTime = (DWORD)(v >> 32); return f; }

BOOL WINAPI FakeGetSystemTimes(PFILETIME idle, PFILETIME kernel, PFILETIME user) {
    *idle = Ft(g_idle); *kernel = Ft(g_kernel); *user = Ft(g_user);
    return TRUE;
}
DWORD WINAPI FakeGetLogicalDrives() {
    DWORD m = 0;
    for (auto& [letter, v] : g_volumes) m |= 1u << (letter - L'A');
    return m;
}
UINT WINAPI FakeGetDriveTypeW(LPCWSTR root) {
    auto it = g_volumes.find(root[0]);
    return it == g_volumes.end() ? DRIVE_NO_ROOT_DIR : it->second.type;
}
HANDLE WINAPI FakeCreateFileW(LPCWSTR path, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
    ++g_opens;
    if (std::wcslen(path) == 6) ++g_volumeOpens;
    HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // a handle of our own: no volume or drive is opened
    g_paths[h] = path;
    return h;
}
BOOL WINAPI FakeDeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD, LPVOID out, DWORD outSize, LPDWORD returned, LPOVERLAPPED) {
    const std::wstring path = g_paths[h];
    if (path.size() == 6 && path.compare(0, 4, L"\\\\.\\") == 0 && path[5] == L':') {   // a volume, \\.\X:
        const FakeVolume& v = g_volumes[path[4]];
        if (code == IOCTL_STORAGE_GET_DEVICE_NUMBER && !v.extentsOnly) {
            auto* n = static_cast<STORAGE_DEVICE_NUMBER*>(out);
            n->DeviceType = FILE_DEVICE_DISK; n->DeviceNumber = v.disk; n->PartitionNumber = 3;
            *returned = sizeof(*n);
            return TRUE;
        }
        if (code == IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS && v.extentsOnly) {
            // spanning two disks, the first extent on v.disk; room for one only would answer ERROR_MORE_DATA
            auto* e = static_cast<VOLUME_DISK_EXTENTS*>(out);
            if (outSize < sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT)) { e->NumberOfDiskExtents = 2; e->Extents[0].DiskNumber = v.disk; SetLastError(ERROR_MORE_DATA); return FALSE; }
            e->NumberOfDiskExtents = 2;
            e->Extents[0].DiskNumber = v.disk;
            e->Extents[1].DiskNumber = v.disk + 1;
            *returned = sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT);
            return TRUE;
        }
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    if (path.rfind(L"\\\\.\\PhysicalDrive", 0) != 0 || code != IOCTL_STORAGE_QUERY_PROPERTY) return FALSE;
    FakeDisk& d = g_disks[_wtoi(path.c_str() + 17)];
    const auto* q = static_cast<const STORAGE_PROPERTY_QUERY*>(in);
    if (q->PropertyId == StorageDeviceTemperatureProperty) {
        ++d.tempQueries;
        if (!d.tempAnswers) return FALSE;
        auto* t = static_cast<STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(out);
        t->Version = t->Size = sizeof(*t);
        t->InfoCount = 1;
        t->TemperatureInfo[0].Temperature = d.temp;
        *returned = d.tempBytes;
        return TRUE;
    }
    if (q->PropertyId == StorageDeviceProperty) {
        ++d.nameQueries;
        auto* s = static_cast<STORAGE_DEVICE_DESCRIPTOR*>(out);
        BYTE* b = static_cast<BYTE*>(out);
        DWORD at = sizeof(STORAGE_DEVICE_DESCRIPTOR) + 8;
        s->BusType = d.bus;
        s->RemovableMedia = d.removable;
        if (*d.vendor) { std::memcpy(b + at, d.vendor, std::strlen(d.vendor) + 1); s->VendorIdOffset = at; at += (DWORD)std::strlen(d.vendor) + 1; }
        std::memcpy(b + at, d.product, std::strlen(d.product) + 1);
        s->ProductIdOffset = at;
        *returned = at + (DWORD)std::strlen(d.product) + 1;
        return TRUE;
    }
    return FALSE;
}
UINT WINAPI FakeGetSystemFirmwareTable(DWORD provider, DWORD, PVOID buf, DWORD size) {
    if (provider != 'RSMB' || g_smbios.empty()) return 0;
    if (buf && size >= g_smbios.size()) std::memcpy(buf, g_smbios.data(), g_smbios.size());
    return (UINT)g_smbios.size();
}
// NVML while a check turns it on (g_nvml): a library of this test's own functions, the readings of an 8 GB
// NVIDIA card. Otherwise no NVIDIA driver; never a dxgi.dll (hw::Video: no adapters).
bool g_nvml = false;
int g_nvmlFieldRc = 3;                 // the memory temperature field's own code: 3 = NVML_ERROR_NOT_SUPPORTED
unsigned g_nvmlMemTemp = 0;            // its value when that code is 0
int g_nvmlFieldCalls = 0;
int g_nvmlMemRc = 0;                   // nvmlDeviceGetMemoryInfo's code (a pass the driver does not answer: not 0)
unsigned long long g_nvmlUsed = 1998261420ull, g_nvmlTotal = 8546984919ull;   // 1.861 of 7.960 GB
struct FakeNvmlMem { unsigned long long total, free, used; };
struct FakeNvmlUtil { unsigned gpu, memory; };
struct FakeNvmlField {
    unsigned fieldId, scopeId;
    long long timestamp, latencyUsec;
    int valueType, nvmlReturn;
    union { double d; unsigned ui; unsigned long long ull; } value;
};
int FakeNvmlHandle(unsigned, void** dev) { *dev = &g_nvml; return 0; }
int FakeNvmlName(void*, char* name, unsigned size) { strcpy_s(name, size, "NVIDIA GeForce RTX 4060 Ti"); return 0; }
int FakeNvmlTemp(void*, int, unsigned* t) { *t = 30; return 0; }
int FakeNvmlUtilization(void*, FakeNvmlUtil* u) { u->gpu = 7; u->memory = 3; return 0; }
int FakeNvmlMemory(void*, FakeNvmlMem* m) { if (g_nvmlMemRc) return g_nvmlMemRc; m->total = g_nvmlTotal; m->used = g_nvmlUsed; m->free = g_nvmlTotal - g_nvmlUsed; return 0; }
int FakeNvmlFields(void*, int count, FakeNvmlField* f) {
    ++g_nvmlFieldCalls;
    if (count != 1 || f->fieldId != 82) return 2;   // NVML_FI_DEV_MEMORY_TEMP only
    f->nvmlReturn = g_nvmlFieldRc;
    f->valueType = 1;                               // NVML_VALUE_TYPE_UNSIGNED_INT
    f->value.ui = g_nvmlMemTemp;
    return 0;                                       // the call itself succeeds, as on the real card
}
// the power calls (the panel's power draw, hw::NvmlPower): mW; a driver may lack either (g_nvmlPowerCalls)
bool g_nvmlPowerCalls = true, g_nvmlLimitCall = true;
unsigned g_nvmlPowerMw = 287000, g_nvmlLimitMw = 360000;   // an RTX 5080 class card: 287 of 360 W
int g_nvmlPowerRc = 0;
int g_nvmlPowerAsks = 0, g_nvmlLimitAsks = 0, g_nvmlInits = 0, g_nvmlLoads = 0;
int FakeNvmlPower(void*, unsigned* mw) { ++g_nvmlPowerAsks; if (g_nvmlPowerRc) return g_nvmlPowerRc; *mw = g_nvmlPowerMw; return 0; }
int FakeNvmlLimit(void*, unsigned* mw) { ++g_nvmlLimitAsks; *mw = g_nvmlLimitMw; return 0; }
int FakeNvmlInitCounted() { ++g_nvmlInits; return 0; }
HMODULE WINAPI FakeLoadLibraryExW(LPCWSTR name, HANDLE, DWORD) {
    if (g_nvml && wcsstr(name, L"nvml.dll")) ++g_nvmlLoads;
    return g_nvml && wcsstr(name, L"nvml.dll") ? (HMODULE)&g_nvml : nullptr;
}
FARPROC WINAPI FakeGetProcAddress(HMODULE lib, LPCSTR name) {
    if (lib != (HMODULE)&g_nvml) return nullptr;
    const std::string n = name;
    if (n == "nvmlInit_v2") return (FARPROC)FakeNvmlInitCounted;
    if (n == "nvmlDeviceGetPowerUsage") return g_nvmlPowerCalls ? (FARPROC)FakeNvmlPower : nullptr;
    if (n == "nvmlDeviceGetEnforcedPowerLimit") return g_nvmlLimitCall ? (FARPROC)FakeNvmlLimit : nullptr;
    if (n == "nvmlDeviceGetHandleByIndex_v2") return (FARPROC)FakeNvmlHandle;
    if (n == "nvmlDeviceGetName") return (FARPROC)FakeNvmlName;
    if (n == "nvmlDeviceGetTemperature") return (FARPROC)FakeNvmlTemp;
    if (n == "nvmlDeviceGetUtilizationRates") return (FARPROC)FakeNvmlUtilization;
    if (n == "nvmlDeviceGetMemoryInfo") return (FARPROC)FakeNvmlMemory;
    if (n == "nvmlDeviceGetFieldValues") return (FARPROC)FakeNvmlFields;
    return nullptr;
}
PDH_STATUS WINAPI FakePdhOpenQueryW(LPCWSTR, DWORD_PTR, PDH_HQUERY*) { return PDH_INVALID_ARGUMENT; }   // no counters

}   // namespace

#define GetSystemTimes FakeGetSystemTimes
#define GetLogicalDrives FakeGetLogicalDrives
#define GetDriveTypeW FakeGetDriveTypeW
#define CreateFileW FakeCreateFileW
#define DeviceIoControl FakeDeviceIoControl
#define GetSystemFirmwareTable FakeGetSystemFirmwareTable
#define LoadLibraryExW FakeLoadLibraryExW
#define GetProcAddress FakeGetProcAddress
#define PdhOpenQueryW FakePdhOpenQueryW
#include "../src/hw_sample.h"
#include "../src/hw_panel.h"
#undef GetSystemTimes
#undef GetLogicalDrives
#undef GetDriveTypeW
#undef CreateFileW
#undef DeviceIoControl
#undef GetSystemFirmwareTable
#undef LoadLibraryExW
#undef GetProcAddress
#undef PdhOpenQueryW

#include "portable_check.h"
#include <cmath>
#include <cwchar>

namespace {

constexpr ULONGLONG kSecond = 10000000;   // 100 ns units
// one second of 8 logical processors: `busy` of them kernel (half) and user (half), the rest idle
void Advance(double busy) {
    const ULONGLONG all = 8 * kSecond, work = (ULONGLONG)(busy * kSecond);
    g_idle += all - work;
    g_kernel += (all - work) + work / 2;   // kernel time includes idle time
    g_user += work - work / 2;
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

// a minute of samples `stepMs` apart from `t` on: how many show the system drive's temperature
int Minute(hw::Sampler& s, ULONGLONG& t, ULONGLONG stepMs, float want) {
    int shown = 0;
    for (ULONGLONG end = t + 60000; t < end; t += stepMs) {
        LDSensorBlock b = {};
        Advance(1.0);
        s.Sample(b, t);
        shown += (b.flags & LD_HAS_SSD_TEMP) && b.ssdTemp == want;
    }
    return shown;
}

void Smbios(std::vector<BYTE>& raw, const std::vector<std::pair<std::string, std::string>>& slots, const std::vector<unsigned>& sizes) {
    std::vector<BYTE> t;
    for (size_t i = 0; i < slots.size(); ++i) {
        std::vector<BYTE> f(0x28, 0);
        f[0] = 17; f[1] = 0x28;
        f[0x0C] = sizes[i] ? 0xFF : 0; f[0x0D] = sizes[i] ? 0x7F : 0;   // 0x7FFF: the extended size
        f[0x0E] = sizes[i] ? 0x09 : 0x02;
        f[0x10] = 1; f[0x11] = 2;
        f[0x1C] = sizes[i] & 0xFF; f[0x1D] = (sizes[i] >> 8) & 0xFF;
        t.insert(t.end(), f.begin(), f.end());
        for (const std::string* s : { &slots[i].first, &slots[i].second }) { t.insert(t.end(), s->begin(), s->end()); t.push_back(0); }
        t.push_back(0);
    }
    t.insert(t.end(), { 127, 4, 0, 0, 0, 0 });
    raw = { 0, 3, 8, 0 };
    const DWORD len = (DWORD)t.size();
    raw.insert(raw.end(), (const BYTE*)&len, (const BYTE*)&len + 4);
    raw.insert(raw.end(), t.begin(), t.end());
}

// HWiNFO's reader as the sensor thread uses it (hwinfo::Reader::Read): the CPU always, the memory
// temperatures only when asked for - a desktop board's, A2 and B2 - and with them the GPUs' memory
// readings a check sets (gpus)
struct FakeDimm { int slot = 0; std::wstring locator; float celsius = 0; };
struct FakeGpu {
    int gpu = 0; std::wstring sensor;
    bool hasTemp = false, hasUsed = false, hasUsage = false;
    float celsius = 0, usedMB = 0, usagePct = 0;
};
struct FakeReading {
    bool running = false, cpuFound = false; float cpuCelsius = 0; std::vector<FakeDimm> dimms; std::vector<FakeGpu> gpus;
    bool cpuPowerFound = false; float cpuPowerW = 0;
};
// the CPU's package power (asked for: Read(..., true)) - reported while cpuPower is set (NAN: not ticked in HWiNFO)
struct FakeHWiNFO {
    int reads = 0, dimmReads = 0, powerReads = 0;
    float cpuPower = NAN;
    std::vector<FakeGpu> gpus;
    FakeReading Read(bool wantDimms, bool wantPower = false) {
        ++reads; dimmReads += wantDimms ? 1 : 0; powerReads += wantPower ? 1 : 0;
        FakeReading r;
        r.running = r.cpuFound = true; r.cpuCelsius = 48.6f;
        if (wantPower && !std::isnan(cpuPower)) { r.cpuPowerFound = true; r.cpuPowerW = cpuPower; }
        if (wantDimms) r.dimms = { { 1, L"P0 CHANNEL A/DIMM 1", 35.8f }, { 3, L"P0 CHANNEL B/DIMM 1", 36.0f } };
        if (wantDimms) r.gpus = gpus;
        return r;
    }
};

int TempQueries() { int n = 0; for (auto& [no, d] : g_disks) n += d.tempQueries; return n; }
int NameQueries() { int n = 0; for (auto& [no, d] : g_disks) n += d.nameQueries; return n; }

// The sensor thread's passes (hw::SensorThread, as sensors.cpp runs them): how long it waits, what it reads
// with the panel closed and open, what the panel gets, the disks let go and their names kept.
void SensorThreadPasses() {
    g_disks.clear();
    g_disks[0].product = "SAMSUNG MZVL21T0HCLR-00B00"; g_disks[0].temp = 39;   // C:, the system drive
    g_disks[1].product = "WD_BLACK SN850X 2000GB"; g_disks[1].temp = 46;       // E:
    g_disks[2].product = "WD_BLACK SN850X 2000GB"; g_disks[2].temp = 42;       // D:
    g_disks[3].bus = BusTypeUsb; g_disks[3].product = "Portable SSD T7";        // G:, USB
    g_volumes.clear();
    g_volumes[L'C'].disk = 0; g_volumes[L'D'].disk = 2; g_volumes[L'E'].disk = 1; g_volumes[L'G'].disk = 3;
    g_volumes[L'Z'].type = DRIVE_REMOTE;
    Smbios(g_smbios, { { "DIMM 0", "P0 CHANNEL A" }, { "DIMM 1", "P0 CHANNEL A" }, { "DIMM 0", "P0 CHANNEL B" }, { "DIMM 1", "P0 CHANNEL B" } },
           { 0, 32768, 0, 32768 });
    Check(hw::CountPanelDrives() == 3, "a window's room before the panel sampled: the 3 internal drives (not the USB one, not the network drive)");

    FakeHWiNFO hwi;
    hw::SensorThread<FakeHWiNFO> t(hwi);
    t.Init();
    Check(t.Cell().SystemDisk() == 0, "the system disk: C:'s, disk 0");
    Check(t.WaitMs(false) == 1000, "the first pass 1 s after the start (the cell filled as soon as before), whatever the panel");

    // the panel closed: every 2 s, the cell only - the system drive every 5 s, no other drive, no memory
    ULONGLONG now = 5000000;
    LDSensorBlock b = {};
    sensorlogic::PanelData p;
    int pass = 0;
    const int opens0 = g_opens;
    for (ULONGLONG end = now + 60000; now < end; now += t.WaitMs(false), ++pass) {
        b = {}; Advance(1.0);
        t.SampleCell(false, now, b);
        if (t.SamplePanel(false, now, p)) pass = -1000;
    }
    Check(t.WaitMs(false) == 2000 && t.WaitMs(true) == 1000, "then every 2 s, every second while a panel is open");
    Check(pass == 30 && g_disks[0].tempQueries == 12 && TempQueries() == 12, "a minute closed: 30 passes, the system drive asked 12 times (every 5 s), no other drive");
    Check(g_opens == opens0 && hwi.dimmReads == 0 && !p.sampled, "...no volume or disk opened, no memory temperatures read, no panel data");
    Check((b.flags & LD_HWINFO) && (b.flags & LD_HAS_CPU_TEMP) && (b.flags & LD_CPU_TEMP_HWINFO) && Near(b.cpuTemp, 48.6f) &&
          (b.flags & LD_HAS_SSD_TEMP) && b.ssdTemp == 39.0f, "the cell's sample: HWiNFO's CPU temperature, the system drive's");

    // the panel open: every second, every drive and the memory, the system drive not asked twice
    for (auto& [no, d] : g_disks) d.tempQueries = 0;
    pass = 0;
    int panels = 0;
    for (ULONGLONG end = now + 30000; now < end; now += t.WaitMs(true), ++pass) {
        b = {}; p = {}; Advance(1.0);
        t.SampleCell(true, now, b);
        panels += t.SamplePanel(true, now, p) ? 1 : 0;
    }
    Check(pass == 30 && panels == 30 && hwi.dimmReads == 30, "30 s open: 30 passes, each with panel data and memory temperatures");
    Check(g_disks[1].tempQueries == 30 && g_disks[2].tempQueries == 30 && g_disks[0].tempQueries <= 31 && g_disks[3].tempQueries == 0,
          "...every internal drive asked once a second; the system drive not twice (at most once more, as it opens); the USB one never");
    Check(p.sampled && p.drives.size() == 3 && p.drives[0].letter == L'C' && p.drives[0].system && p.drives[1].letter == L'D' &&
          p.drives[1].model == L"WD SN850X" && Near(p.drives[1].temp, 42) && p.drives[2].letter == L'E' && Near(p.drives[2].temp, 46) &&
          p.drivesWithTemp == 3, "the panel's drives: C: (system), D: 42 °C, E: 46 °C by letter, 3 with a temperature");
    Check(p.dimms.size() == 2 && p.dimms[0].label == L"RAM 1 · A2" && Near(p.dimms[0].temp, 35.8f) && p.dimms[1].label == L"RAM 2 · B2" &&
          p.modules == 2 && p.dimmsWithTemp == 2, "...its memory: RAM 1 · A2 35.8 °C, RAM 2 · B2, 2 of 2 modules");
    Check(!p.vram.present && g_nvmlFieldCalls == 0, "...no VRAM block: no NVML, no adapter with memory of its own (the panel as before)");
    g_disks[0].temp = 44;
    b = {}; Advance(1.0);
    t.SampleCell(true, now, b); t.SamplePanel(true, now, p);
    now += 1000; b = {}; Advance(1.0);
    t.SampleCell(true, now, b);
    Check(b.ssdTemp == 44.0f, "the cell's system drive: the list's reading of the pass before (44 °C)");

    // closed: the disks let go, the drives no longer asked; opened again with the same letters: the disks
    // opened again, but no volume looked up and no name asked again
    p = {};
    Check(!t.SamplePanel(false, now, p) && !p.sampled, "the panel closes: no panel data");
    for (auto& [no, d] : g_disks) d.tempQueries = 0;
    for (ULONGLONG end = now + 20000; now < end; now += t.WaitMs(false)) { b = {}; Advance(1.0); t.SampleCell(false, now, b); t.SamplePanel(false, now, p); }
    Check(TempQueries() == g_disks[0].tempQueries && g_disks[0].tempQueries <= 5, "...20 s closed: only the system drive asked (every 5 s)");
    const int names = NameQueries(), volumes = g_volumeOpens, opens = g_opens;
    t.SampleCell(true, now, b);
    Check(t.SamplePanel(true, now, p) && p.drives.size() == 3, "opened again: the same three drives");
    Check(NameQueries() == names && g_volumeOpens == volumes && g_opens == opens + 3,
          "...the same letters: the three disks opened again, no volume looked up, no name asked (kept from the last opening)");
    t.SamplePanel(false, now, p);
    g_volumes[L'F'].disk = 4; g_disks[4].product = "Samsung SSD 870 EVO 1TB"; g_disks[4].bus = BusTypeSata;
    t.SampleCell(true, now, b);
    Check(t.SamplePanel(true, now, p) && p.drives.size() == 4 && p.drives[3].model == L"Samsung 870 EVO" && NameQueries() > names,
          "a letter added since: everything asked again, the new drive listed");
}

// What the sensor settings leave out (hw::SensorThread::Want, sensors.cpp sets it before every pass): HWiNFO
// not the source - not read at all, nothing of it in the sample or the panel; the drives hidden - neither the
// system drive's temperature (every 5 s) nor the panel's (every second) asked, no disk held open; shown again,
// everything as before.
void WantPasses() {
    g_disks.clear();
    g_disks[0].product = "SAMSUNG MZVL21T0HCLR-00B00"; g_disks[0].temp = 39;   // C:, the system drive
    g_disks[1].product = "WD_BLACK SN850X 2000GB"; g_disks[1].temp = 46;       // D:
    g_volumes.clear();
    g_volumes[L'C'].disk = 0; g_volumes[L'D'].disk = 1;
    FakeHWiNFO hwi;   // the memory: the slots read once for the process (SensorThreadPasses' table)
    hw::SensorThread<FakeHWiNFO> t(hwi);
    t.Init();
    ULONGLONG now = 9000000;
    LDSensorBlock b = {};
    sensorlogic::PanelData p;

    // HWiNFO not the source (temperatures off, or PawnIO): a minute closed and 10 s open without one read
    t.Want(false, true);
    for (ULONGLONG end = now + 60000; now < end; now += t.WaitMs(false)) { b = {}; Advance(1.0); t.SampleCell(false, now, b); t.SamplePanel(false, now, p); }
    Check(hwi.reads == 0 && !(b.flags & (LD_HWINFO | LD_HAS_CPU_TEMP | LD_CPU_TEMP_HWINFO)) && (b.flags & LD_HAS_SSD_TEMP),
          "HWiNFO not the source: never read (no registry or process polling); no CPU temperature, the system drive's still there");
    for (ULONGLONG end = now + 10000; now < end; now += t.WaitMs(true)) { b = {}; p = {}; Advance(1.0); t.SampleCell(true, now, b); t.SamplePanel(true, now, p); }
    Check(hwi.reads == 0 && p.sampled && p.drives.size() == 2 && !p.dimms.empty() && std::isnan(p.dimms[0].temp) && p.dimmsWithTemp == 0,
          "...the panel open: still not read - its drives, its module without a temperature (no HWiNFO)");
    t.Want(true, true);
    b = {}; Advance(1.0); t.SampleCell(true, now, b);
    Check(hwi.reads == 1 && (b.flags & LD_CPU_TEMP_HWINFO) && Near(b.cpuTemp, 48.6f), "the source HWiNFO again: read at the next pass");
    t.SamplePanel(false, now, p);

    // the drives hidden: neither asked nor held
    t.Want(true, false);
    for (auto& [no, d] : g_disks) d.tempQueries = 0;
    for (ULONGLONG end = now + 60000; now < end; now += t.WaitMs(false)) { b = {}; Advance(1.0); t.SampleCell(false, now, b); t.SamplePanel(false, now, p); }
    Check(TempQueries() == 0 && !(b.flags & LD_HAS_SSD_TEMP), "SSD hidden: a minute closed without the system drive asked (was every 5 s), no SSD temperature in the sample");
    const int opens = g_opens;
    int panels = 0;
    for (ULONGLONG end = now + 10000; now < end; now += t.WaitMs(true)) { b = {}; p = {}; Advance(1.0); t.SampleCell(true, now, b); panels += t.SamplePanel(true, now, p) ? 1 : 0; }
    Check(panels == 10 && TempQueries() == 0 && g_opens == opens && p.sampled && p.drives.empty() && p.drivesWithTemp == 0 && !p.dimms.empty(),
          "...10 s open: panel data without drives, no disk opened or asked (the memory as before)");
    t.Want(true, true);
    b = {}; p = {}; Advance(1.0); t.SampleCell(true, now, b);
    Check(t.SamplePanel(true, now, p) && p.drives.size() == 2 && Near(p.drives[1].temp, 46) && TempQueries() >= 2,
          "shown again while open: the drives looked for and asked at once");
    t.SamplePanel(false, now, p);
}

// The panel's VRAM block as the sensor thread fills it (hw::SensorThread::SamplePanel): NVML's amounts, the
// memory's temperature from NVML where the GPU has one and otherwise from HWiNFO, and a GPU without NVML.
void VramPasses() {
    g_disks.clear(); g_disks[0].product = "SAMSUNG MZVL21T0HCLR-00B00";
    g_volumes.clear(); g_volumes[L'C'].disk = 0;
    const wchar_t* aero = L"dGPU [#0]: NVIDIA GeForce RTX 4060 Ti: ASUS Dual GeForce RTX 4060 Ti OC 8G (DUAL-RTX4060TI-O8G)";
    Check(!hw::Video().nvidia && hw::Video().dedicatedGB == 0 && hw::Video().name.empty(), "no dxgi.dll for this test: no adapters looked at");
    ULONGLONG now = 9000000;
    LDSensorBlock b = {};
    sensorlogic::PanelData p;
    auto pass = [&](auto& t, bool open) {
        b = {}; p = {}; Advance(1.0);
        t.SampleCell(open, now, b);
        const bool got = t.SamplePanel(open, now, p);
        now += t.WaitMs(open);
        return got;
    };

    // an NVIDIA card: NVML's amounts, no NVML memory temperature (not supported), HWiNFO's junction temperature
    {
        g_nvml = true; g_nvmlFieldRc = 3; g_nvmlFieldCalls = 0;
        FakeHWiNFO hwi;
        FakeGpu g; g.gpu = 0; g.sensor = aero; g.hasTemp = true; g.celsius = 44.0f;
        hwi.gpus = { g };
        hw::SensorThread<FakeHWiNFO> t(hwi);
        t.Init(hw::VideoInfo{ true, 7.706f, L"NVIDIA GeForce RTX 4060 Ti" });
        for (int i = 0; i < 30; ++i) if (pass(t, false)) now = 0;
        Check(now != 0 && g_nvmlFieldCalls == 0 && (b.flags & LD_HAS_GPU) && Near(b.gpuMemUsed, 1.861f) && Near(b.gpuMemTotal, 7.960f),
              "a minute closed with NVML: the cell's GPU readings (1.861 of 7.960 GB), its memory temperature never asked");
        Check(pass(t, true) && p.vram.present && Near(p.vram.usedGB, 1.861f) && Near(p.vram.totalGB, 7.960f) && std::fabs(p.vram.load - 23.38f) < 0.05f,
              "the panel opens: the VRAM block with NVML's amounts (1.9 of 8.0 GB, 23 %)");
        Check(Near(p.vram.temp, 44.0f) && g_nvmlFieldCalls == 1, "...its temperature HWiNFO's (44.0 °C): NVML asked once, not supported");
        g_nvmlUsed = 4294967296ull;   // 4 GB in use now
        for (int i = 0; i < 30; ++i) pass(t, true);
        Check(p.vram.present && Near(p.vram.usedGB, 4.0f) && Near(p.vram.temp, 44.0f) && g_nvmlFieldCalls == 1,
              "30 s open: the amount follows NVML every pass (4.0 GB), NVML's memory temperature not asked again");
        hwi.gpus.clear();
        Check(pass(t, true) && p.vram.present && std::isnan(p.vram.temp) && Near(p.vram.usedGB, 4.0f), "HWiNFO's reading unticked: the block stays, its temperature unknown (\"--°C\")");
        // a pass NVML does not answer for its memory (a driver restart): the block and its size stay - not the
        // adapter's 7.7 GB for a second -, the amount unknown for that pass
        g_nvmlMemRc = 999;
        Check(pass(t, true) && p.vram.present && Near(p.vram.totalGB, 7.960f) && std::isnan(p.vram.usedGB) && std::isnan(p.vram.load),
              "a pass without NVML's memory answer: the block keeps NVML's size (8.0 GB), \"--\" in use");
        g_nvmlMemRc = 0;
        Check(pass(t, true) && Near(p.vram.totalGB, 7.960f) && Near(p.vram.usedGB, 4.0f), "...and its amount again at the next");
        g_nvmlUsed = 1998261420ull;
    }
    // a GPU whose NVML reports the memory temperature: NVML's, asked every pass while the panel is open
    {
        g_nvmlFieldRc = 0; g_nvmlMemTemp = 58; g_nvmlFieldCalls = 0;
        FakeHWiNFO hwi;
        FakeGpu g; g.gpu = 0; g.sensor = aero; g.hasTemp = true; g.celsius = 44.0f;
        hwi.gpus = { g };
        hw::SensorThread<FakeHWiNFO> t(hwi);
        t.Init(hw::VideoInfo{ true, 7.706f, L"NVIDIA GeForce RTX 4060 Ti" });
        for (int i = 0; i < 10; ++i) pass(t, true);
        Check(Near(p.vram.temp, 58.0f) && g_nvmlFieldCalls == 10, "NVML reports the memory temperature (58 °C): shown instead of HWiNFO's, asked every open pass");
        g_nvmlFieldRc = 999;   // one pass without an answer
        pass(t, true);
        Check(Near(p.vram.temp, 44.0f), "...a pass NVML does not answer: HWiNFO's that pass");
        g_nvmlFieldRc = 0;
        pass(t, true);
        Check(Near(p.vram.temp, 58.0f) && g_nvmlFieldCalls == 12, "...and NVML's again at the next (it answered before: still asked)");
        g_nvmlMemTemp = 0;     // an answer that is no temperature
        pass(t, true);
        Check(Near(p.vram.temp, 44.0f), "0 °C from NVML is no temperature: HWiNFO's");
        g_nvmlMemTemp = 58;
        for (int i = 0; i < 5; ++i) pass(t, false);
        Check(g_nvmlFieldCalls == 13, "closed: not asked");
    }
    g_nvml = false; g_nvmlFieldRc = 3; g_nvmlFieldCalls = 0;
    // a GPU NVML does not know: the adapter's size, HWiNFO's amount and temperature for the GPU of that name
    {
        FakeHWiNFO hwi;
        FakeGpu other; other.gpu = 0; other.sensor = L"iGPU [#0]: AMD Radeon 780M"; other.hasUsed = true; other.usedMB = 420; other.hasTemp = true; other.celsius = 71;
        FakeGpu g; g.gpu = 1; g.sensor = L"dGPU [#1]: AMD Radeon RX 7900 XT: Sapphire"; g.hasUsed = true; g.usedMB = 3412; g.hasTemp = true; g.celsius = 62;
        hwi.gpus = { other, g };
        hw::SensorThread<FakeHWiNFO> t(hwi);
        t.Init(hw::VideoInfo{ false, 19.93f, L"AMD Radeon RX 7900 XT" });
        Check(pass(t, true) && p.vram.present && Near(p.vram.totalGB, 19.93f) && std::fabs(p.vram.usedGB - 3.332f) < 0.001f && Near(p.vram.temp, 62.0f) &&
              g_nvmlFieldCalls == 0, "no NVML, a 20 GB adapter: its size, HWiNFO's 3,412 MB and 62 °C of the GPU of that name (not the integrated one's)");
        hwi.gpus.clear();
        Check(pass(t, true) && p.vram.present && std::isnan(p.vram.usedGB) && std::isnan(p.vram.temp), "...nothing from HWiNFO: the block with \"--\"");
    }
    // NVML that never gave a memory size, on a PC whose first adapter has little memory of its own: no block
    {
        g_nvml = true; g_nvmlMemRc = 999;
        FakeHWiNFO hwi;
        hw::SensorThread<FakeHWiNFO> t(hwi);
        t.Init(hw::VideoInfo{ true, 0.125f, L"Intel Arc 140V GPU" });
        Check(pass(t, true) && p.sampled && !p.vram.present, "NVML without a memory answer from the start, a 128 MB adapter: no block");
        g_nvmlMemRc = 0;
        Check(pass(t, true) && p.vram.present && Near(p.vram.totalGB, 7.960f), "...the block once it answers");
        g_nvmlMemRc = 999;
        Check(pass(t, true) && p.vram.present && Near(p.vram.totalGB, 7.960f) && std::isnan(p.vram.usedGB), "...and it does not go again with a pass that fails (the panel's height stays)");
        g_nvml = false; g_nvmlMemRc = 0;
    }
    // an integrated GPU: no block, whatever HWiNFO reports
    {
        FakeHWiNFO hwi;
        FakeGpu g; g.gpu = 0; g.sensor = L"GPU [#0]: Intel Arc 140V GPU"; g.hasUsed = true; g.usedMB = 900;
        hwi.gpus = { g };
        hw::SensorThread<FakeHWiNFO> t(hwi);
        t.Init(hw::VideoInfo{ false, 0.125f, L"Intel Arc 140V GPU (16GB)" });
        Check(pass(t, true) && p.sampled && !p.vram.present, "an integrated GPU with 128 MB of dedicated memory: no VRAM block");
    }
}

// The panel's power draw as the sensor thread reads it (hw::SensorThread::WantPower, SamplePower): 없음 reads
// nothing new (not even NVML's power calls looked up), 그래프 every pass into a 60 s history placed by time (2 s
// apart closed, 1 s open), 요약 only while a panel is open; a device switched off, or the view leaving 그래프,
// drops what it held; the GPU's top is NVML's enforced limit (read every 30 s and when a panel opens), the CPU's
// the steady auto scale; no NVML power call, or no NVML, gives no GPU power and nothing breaks.
void PowerPasses() {
    using namespace sensorlogic;
    g_disks.clear(); g_disks[0].product = "SAMSUNG MZVL21T0HCLR-00B00";
    g_volumes.clear(); g_volumes[L'C'].disk = 0;
    ULONGLONG now = 20000000;
    LDSensorBlock b = {};
    sensorlogic::PanelData p;
    sensorlogic::PowerData pw;
    int published = 0;
    auto pass = [&](auto& t, bool open) {
        b = {}; Advance(1.0);
        t.SampleCell(open, now, b);
        const bool got = t.SamplePower(open, now, pw);
        published += got ? 1 : 0;
        t.SamplePanel(open, now, p);
        now += t.WaitMs(open);
        return got;
    };
    g_nvml = true; g_nvmlPowerCalls = g_nvmlLimitCall = true; g_nvmlPowerRc = 0; g_nvmlPowerMw = 287000; g_nvmlLimitMw = 360000;
    g_nvmlPowerAsks = g_nvmlLimitAsks = 0;
    {
        FakeHWiNFO hwi;
        hwi.cpuPower = 71.2f;
        hw::SensorThread<FakeHWiNFO> t(hwi);
        t.Init(hw::VideoInfo{ true, 15.6f, L"NVIDIA GeForce RTX 5080" });
        const int loads = g_nvmlLoads, inits = g_nvmlInits;

        // 없음 (the default): a minute closed and 10 s open, nothing new read or published
        t.Want(true, true);
        t.WantPower(kPowerOff, false, false);
        for (int i = 0; i < 30; ++i) pass(t, false);
        for (int i = 0; i < 10; ++i) pass(t, true);
        Check(published == 0 && hwi.powerReads == 0 && g_nvmlPowerAsks == 0 && g_nvmlLimitAsks == 0 && g_nvmlLoads == loads && g_nvmlInits == inits,
              "없음: 40 passes (closed and open) - no HWiNFO power asked, NVML's power calls neither looked up nor asked, nothing published");
        pass(t, false);   // closed again (the sampler's own linger is SensorClient's: here the next pass is closed)

        // 그래프, both: every pass, closed (2 s apart) or open (1 s); one handle of its own on NVML, made once
        t.WantPower(kPowerGraph, true, true);
        published = 0;
        const ULONGLONG start = now;
        for (int i = 0; i < 30; ++i) pass(t, false);
        Check(published == 30 && hwi.powerReads == 30 && g_nvmlPowerAsks == 30 && g_nvmlLoads == loads + 1 && g_nvmlInits == inits + 1,
              "그래프: a minute closed - 30 passes, each with the CPU's (HWiNFO) and the GPU's (NVML) power; NVML loaded and initialised once more, once");
        Check(pw.view == kPowerGraph && pw.cpu.read && Near(pw.cpu.watts, 71.2f) && pw.gpu.read && Near(pw.gpu.watts, 287.0f) && Near(pw.gpu.limitW, 360.0f),
              "...CPU 71.2 W, GPU 287 W of its 360 W limit");
        Check(g_nvmlLimitAsks == 2, "...the GPU's enforced limit read at the first pass and 30 s on (2 in a minute), not every pass");
        Check(pw.atMs == now - 2000 && pw.cpu.history.size() == 30 && pw.cpu.history.back().ms == pw.atMs && pw.cpu.history.front().ms == start,
              "...the history: the minute's 30 points 2 s apart by time, the newest at the pass's time");
        Check(Near(pw.gpu.top, 360.0f) && Near(pw.cpu.top, 100.0f), "...the graph's tops: the GPU's limit (360 W), the CPU's auto scale (71.2 W with 20 % room, 85.4 -> 100 W)");
        for (int i = 0; i < 20; ++i) pass(t, true);
        size_t inWindow = 0;
        for (const auto& pt : pw.cpu.history) inWindow += pt.ms + kPowerWindowMs >= pw.atMs ? 1 : 0;
        Check(published == 50 && pw.cpu.history.size() >= 40 && inWindow == pw.cpu.history.size() - 1,
              "...20 s open: a point every second, mixed with the 2 s ones before (placed by time), still only the last 60 s");
        Check(g_nvmlLimitAsks == 3, "...the panel opening read the limit again (3), then not for 30 s");
        hwi.cpuPower = 140.f;
        pass(t, true);
        Check(Near(pw.cpu.top, 200.0f), "a CPU peak of 140 W: the auto top rises at once (168 -> 200 W)");
        hwi.cpuPower = 60.f;
        for (int i = 0; i < 30; ++i) pass(t, true);
        Check(Near(pw.cpu.top, 200.0f), "...30 s later the peak is still in the window: the top stays");
        for (int i = 0; i < 35; ++i) pass(t, true);
        Check(Near(pw.cpu.top, 100.0f), "...the peak left the 60 s window: down at once to 100 W (60 W steady keeps 35 % of room: 81 -> 100)");

        // a CPU package power no longer reported (unticked in HWiNFO): unknown, no point added, the history moves on
        hwi.cpuPower = NAN;
        pass(t, true);
        Check(pw.cpu.read && std::isnan(pw.cpu.watts) && pw.cpu.history.back().ms < pw.atMs && Near(pw.gpu.watts, 287.0f),
              "the CPU's power unticked: read but unknown (the card's hint), no new point; the GPU's goes on");
        hwi.cpuPower = 65.f;

        // NVML does not answer for a pass: the GPU's unknown for it, its limit kept
        g_nvmlPowerRc = 999;
        pass(t, true);
        Check(std::isnan(pw.gpu.watts) && Near(pw.gpu.limitW, 360.0f) && Near(pw.gpu.top, 360.0f), "a pass NVML does not answer: the GPU's unknown for it, the limit and the top kept");
        g_nvmlPowerRc = 0;

        // GPU 전력 off: its all dropped at once, NVML no longer asked; on again: the limit read at once
        t.WantPower(kPowerGraph, true, false);
        const int asks = g_nvmlPowerAsks, limits = g_nvmlLimitAsks;
        Check(pass(t, true) && !pw.gpu.read && std::isnan(pw.gpu.watts) && pw.gpu.history.empty() && pw.gpu.top == 0 && std::isnan(pw.gpu.limitW) && pw.cpu.read,
              "GPU 전력 off: the GPU's power, limit and history dropped, the CPU's kept");
        for (int i = 0; i < 10; ++i) pass(t, true);
        Check(g_nvmlPowerAsks == asks && g_nvmlLimitAsks == limits, "...and NVML not asked for 10 passes");
        t.WantPower(kPowerGraph, true, true);
        pass(t, true);
        Check(g_nvmlLimitAsks == limits + 1 && Near(pw.gpu.limitW, 360.0f) && pw.gpu.history.size() == 1, "...on again: the limit read at its first pass, a new history");

        // 요약: the histories go; read only while a panel is open
        t.WantPower(kPowerSummary, true, true);
        Check(pass(t, true) && pw.view == kPowerSummary && pw.cpu.history.empty() && pw.cpu.top == 0 && pw.gpu.history.empty() && Near(pw.cpu.watts, 65.0f),
              "요약: the histories and tops dropped, the current watts read (panel open)");
        published = 0;
        const int reads = hwi.powerReads, gpuAsks = g_nvmlPowerAsks;
        for (int i = 0; i < 30; ++i) pass(t, false);
        Check(published == 0 && hwi.powerReads == reads && g_nvmlPowerAsks == gpuAsks,
              "...a minute closed: neither power asked, nothing published (the footer it feeds is not seen)");
        Check(pass(t, true) && published == 1 && hwi.powerReads == reads + 1 && g_nvmlPowerAsks == gpuAsks + 1 && pw.cpu.history.empty(),
              "...the panel opens: both read on that pass, no history kept");

        // the source not HWiNFO (WantPower says the CPU's is not shown): no HWiNFO read for it
        t.Want(false, true);
        t.WantPower(kPowerSummary, false, true);
        Check(pass(t, true) && !pw.cpu.read && pw.gpu.read && hwi.powerReads == reads + 1, "PawnIO / 끄기: no CPU power (HWiNFO only), the GPU's still read");
        t.Want(true, true);

        // 없음 again: what was there dropped once, then nothing
        t.WantPower(kPowerOff, false, false);
        published = 0;
        Check(pass(t, true) && pw.view == kPowerOff && !pw.cpu.read && !pw.gpu.read, "없음 again: everything dropped on its first pass");
        for (int i = 0; i < 10; ++i) pass(t, true);
        Check(published == 1, "...and nothing published after it");
    }
    // a driver without the power calls, or no NVML at all: no GPU power, nothing else changes
    {
        g_nvmlPowerCalls = false; g_nvmlLimitCall = false;
        FakeHWiNFO hwi;
        hw::SensorThread<FakeHWiNFO> t(hwi);
        t.Init(hw::VideoInfo{ true, 15.6f, L"NVIDIA GeForce RTX 5080" });
        t.Want(true, true);
        t.WantPower(kPowerGraph, true, true);
        for (int i = 0; i < 3; ++i) pass(t, false);
        Check(pw.gpu.read && std::isnan(pw.gpu.watts) && std::isnan(pw.gpu.limitW) && pw.gpu.history.empty() && (b.flags & LD_HAS_GPU) && Near(b.gpuTemp, 30.0f),
              "an NVML without nvmlDeviceGetPowerUsage: GPU power unknown, the cell's GPU readings as before");
        Check(pw.cpu.read && std::isnan(pw.cpu.watts) && pw.cpu.history.empty() && pw.cpu.top == 0, "...and HWiNFO without a CPU package power: unknown, no history, no top");
        g_nvmlPowerCalls = true; g_nvmlLimitCall = false;
        hw::SensorThread<FakeHWiNFO> t2(hwi);
        t2.Init(hw::VideoInfo{ true, 15.6f, L"NVIDIA GeForce RTX 5080" });
        t2.Want(true, true);
        t2.WantPower(kPowerGraph, false, true);
        for (int i = 0; i < 3; ++i) pass(t2, false);
        Check(Near(pw.gpu.watts, 287.0f) && std::isnan(pw.gpu.limitW) && Near(pw.gpu.top, 400.0f),
              "...without nvmlDeviceGetEnforcedPowerLimit: the power, and the auto scale for its top (287 W with 20 % room -> 400 W)");
        g_nvml = false; g_nvmlLimitCall = true;
        const int loads = g_nvmlLoads;
        hw::SensorThread<FakeHWiNFO> t3(hwi);
        t3.Init(hw::VideoInfo{ false, 0.125f, L"Intel Arc 140V GPU" });
        t3.Want(true, true);
        t3.WantPower(kPowerGraph, false, true);
        for (int i = 0; i < 3; ++i) pass(t3, false);
        Check(pw.gpu.read && std::isnan(pw.gpu.watts) && g_nvmlLoads == loads, "no NVML (a laptop's Intel GPU): GPU power unknown, nvml.dll not looked for again");
    }
}

}   // namespace

int main() {
    g_idle = 123 * kSecond; g_kernel = 456 * kSecond; g_user = 789 * kSecond;   // since boot
    hw::Sampler sampler;
    sampler.Init();
    Check(sampler.SystemDisk() == 1, "the system disk: the number of the Windows folder's volume (1)");

    // ---- the CPU load
    LDSensorBlock s = {};
    Advance(2.0);
    sampler.Sample(s);
    Check(Near(s.cpuLoad, 25.0f), "the first sample: measured from Init, 2 of 8 processors busy for the second = 25 %");
    s = {}; Advance(8.0); sampler.Sample(s);
    Check(Near(s.cpuLoad, 100.0f), "every processor busy: 100 %");
    s = {}; Advance(0.0); sampler.Sample(s);
    Check(Near(s.cpuLoad, 0.0f), "every processor idle: 0 %");
    s = {}; sampler.Sample(s);
    Check(s.cpuLoad == 0.0f, "no time passed between two samples (the times did not move): 0 %, not a division by zero");
    s = {}; Advance(3.0); sampler.Sample(s);
    Check(Near(s.cpuLoad, 37.5f), "...and the next sample is measured from that one (3 of 8: 37.5 %)");

    // ---- the system drive's temperature: every 5 s by the clock, the last answer standing in between
    {
        FakeDisk& sys = g_disks[1];
        hw::Sampler fresh;
        fresh.Init();
        ULONGLONG t = 1000000;
        sys.tempQueries = 0;
        int shown = Minute(fresh, t, 2000, 41.0f);
        Check(sys.tempQueries == 12, "a minute of samples 2 s apart (panel closed): the temperature asked 12 times (every 5 s, not every 5th sample = 10 s)");
        Check(shown == 30, "...and every sample shows it (the last answer stands in between)");
        sys.tempQueries = 0;
        shown = Minute(fresh, t, 1000, 41.0f);
        Check(sys.tempQueries == 12 && shown == 60, "a minute 1 s apart (panel open, the drive not in its list): still 12, every sample shows it");

        // the panel's list reads the system drive every second: the sampler does not ask it too
        sys.tempQueries = 0;
        for (ULONGLONG end = t + 60000; t < end; t += 1000) {
            fresh.SetSystemDriveTemp(true, 44.0f, t);
            LDSensorBlock b = {}; Advance(1.0); fresh.Sample(b, t);
            if (!(b.flags & LD_HAS_SSD_TEMP) || b.ssdTemp != 44.0f) shown = -1;
        }
        Check(sys.tempQueries == 0 && shown != -1, "a minute of the panel's list reading it (44 °C): not asked by the sampler, the list's value shown");
        LDSensorBlock b = {};
        t += 2000; fresh.Sample(b, t);
        Check(sys.tempQueries == 0, "the panel closes: 2 s later not asked yet");
        t += 2000; b = {}; fresh.Sample(b, t);
        Check(sys.tempQueries == 1 && b.ssdTemp == 41.0f, "...5 s after the list's last reading it is (41 °C)");
        fresh.SetSystemDriveTemp(false, NAN, t);
        b = {}; fresh.Sample(b, t + 1000);
        Check(!(b.flags & LD_HAS_SSD_TEMP), "the list found no temperature for it: none shown");

        // the drive stops answering: not shown from the next query on
        sys.tempAnswers = false;
        t += 10000;
        shown = Minute(fresh, t, 2000, 41.0f);
        Check(shown == 0, "a drive that no longer answers: no temperature from the sample that asked on");
        sys.tempAnswers = true; sys.temp = 150;
        shown = Minute(fresh, t, 2000, 150.0f);
        Check(shown == 0, "an implausible 150 °C: not shown");
        sys.temp = 39; sys.tempBytes = 8;   // an answer shorter than its first temperature
        shown = Minute(fresh, t, 2000, 39.0f);
        Check(shown == 0, "an answer too short to hold a temperature: not shown");
        sys.tempBytes = sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR);
        b = {};
        for (int i = 0; i < 3; ++i) { b = {}; Advance(1.0); t += 2000; fresh.Sample(b, t); }
        Check((b.flags & LD_HAS_SSD_TEMP) && b.ssdTemp == 39.0f, "...a plausible one again: shown again at the next query");
        sys.temp = 41;
    }

    // ---- names, GPU without NVML, nothing the panel no longer shows
    {
        LDSensorBlock b = {};
        Advance(1.0);
        sampler.Sample(b);
        Check(std::wcscmp(b.gpuName, L"GPU") == 0 && !(b.flags & (LD_HAS_GPU | LD_HAS_GPU_LOAD)), "no NVIDIA driver and no counters: \"GPU\", no GPU readings");
        Check(b.cpuName[0] != 0 && b.cpuName[63] == 0, "the CPU's name is there and fits its field");
        Check(b.ssdName[0] == 0 && b.ssdActive == 0 && b.ssdUsed == 0 && b.ssdTotal == 0,
              "the drive's activity, space and name are no longer sampled (nothing shows them; the panel lists the drives)");
    }

    // ---- memory: this PC's, read as it is
    {
        LDSensorBlock b = {};
        Advance(1.0);
        sampler.Sample(b);
        Check(b.ramTotal > 0.5f && b.ramUsed > 0 && b.ramUsed <= b.ramTotal && b.ramLoad > 0 && b.ramLoad <= 100,
              "memory: total, used and load of this PC in range");
    }

    // ---- the panel's drive list
    {
        g_disks.clear();
        g_disks[0].product = "SAMSUNG MZVL21T0HCLR-00B00";
        g_disks[1].product = "WD_BLACK SN850X 2000GB"; g_disks[1].temp = 43;
        g_disks[2].product = "WD_BLACK SN850X 2000GB"; g_disks[2].tempAnswers = false;
        g_disks[3].bus = BusTypeUsb; g_disks[3].product = "Portable SSD T7";
        g_disks[4].bus = BusTypeFileBackedVirtual; g_disks[4].product = "Virtual Disk";
        g_disks[5].bus = BusTypeSata; g_disks[5].product = "Samsung SSD 870 EVO 1TB"; g_disks[5].temp = 29;
        g_disks[6].bus = BusTypeSd; g_disks[6].removable = true; g_disks[6].product = "SD Card";
        g_disks[7].bus = BusTypeSpaces; g_disks[7].product = "Storage Space";
        g_volumes.clear();
        g_volumes[L'E'].disk = 1;          // E: and F: on disk 1: one row, E:
        g_volumes[L'F'].disk = 1;
        g_volumes[L'C'].disk = 0;
        g_volumes[L'D'].disk = 2;
        g_volumes[L'G'].disk = 3;          // USB
        g_volumes[L'H'].disk = 4;          // a mounted VHD
        g_volumes[L'I'].disk = 5; g_volumes[L'I'].extentsOnly = true;   // a spanned dynamic volume, first extent on disk 5
        g_volumes[L'J'].disk = 6;          // an SD card (fixed by its driver, removable media)
        g_volumes[L'K'].disk = 7;          // Storage Spaces
        g_volumes[L'L'].type = DRIVE_CDROM;
        g_volumes[L'Z'].type = DRIVE_REMOTE;
        hw::DriveList list;
        Check(list.Refresh(0, true), "the panel opens: the list is made");
        const auto& rows = list.Rows();
        std::wstring got;
        for (auto& r : rows) { got += r.letter; got += L'='; got += std::to_wstring(r.disk); got += L' '; }
        std::wprintf(L"  drives: %ls\n", got.c_str());
        Check(rows.size() == 4 && rows[0].letter == L'C' && rows[1].letter == L'D' && rows[2].letter == L'E' && rows[3].letter == L'I',
              "internal drives by letter: C, D, E (not F: same disk), I (a spanned volume by its first extent); not USB, VHD, SD, Storage Spaces, CD, network");
        Check(rows.size() == 4 && rows[0].system && !rows[1].system && rows[0].disk == 0 && rows[3].disk == 5, "the system disk marked, disk numbers kept");
        Check(rows.size() == 4 && rows[0].model == L"Samsung MZVL21T0HCLR-00B00" && rows[1].model == L"WD SN850X" && rows[3].model == L"Samsung 870 EVO",
              "cleaned names: Samsung MZVL21T0HCLR-00B00, WD SN850X, Samsung 870 EVO");
        Check(rows.size() == 4 && std::isnan(rows[0].temp), "no temperature before ReadTemps");
        list.ReadTemps();
        Check(rows.size() == 4 && Near(rows[0].temp, 41) && std::isnan(rows[1].temp) && Near(rows[2].temp, 43) && Near(rows[3].temp, 29),
              "temperatures: 41, -- (no answer), 43, 29");
        Check(sensorlogic::CountWithTemp(rows) == 3, "footer: SSD 3/4");
        const int opens = g_opens;
        Check(!list.Refresh(0, false) && g_opens == opens, "a second later, the same letters: not looked for again (nothing opened)");
        g_volumes[L'M'].disk = 8; g_disks[8].product = "KINGSTON SA400S37480G"; g_disks[8].bus = BusTypeSata;
        Check(list.Refresh(0, false) && list.Rows().size() == 5 && list.Rows()[4].model == L"Kingston SA400S37480G",
              "a drive letter added while the panel is open: looked for again, the new drive listed");
        list.Close();
        Check(list.Rows().empty(), "the panel closes: the disks let go");
        Check(list.Refresh(0, false) && list.Rows().size() == 5, "it opens again: looked for again");
    }

    // ---- the memory slots (SMBIOS type 17), a desktop board's layout
    {
        Smbios(g_smbios, { { "DIMM 0", "P0 CHANNEL A" }, { "DIMM 1", "P0 CHANNEL A" }, { "DIMM 0", "P0 CHANNEL B" }, { "DIMM 1", "P0 CHANNEL B" } },
               { 0, 32768, 0, 32768 });
        auto mods = hw::ReadMemoryModules();
        Check(mods.size() == 4 && sensorlogic::InstalledModules(mods) == 2 && mods[1].bank == L"P0 CHANNEL A" && mods[3].device == L"DIMM 1",
              "SMBIOS: 4 slots, 2 modules (A2, B2)");
        g_smbios.clear();
        Check(hw::ReadMemoryModules().empty(), "no SMBIOS table: no slots");
        Smbios(g_smbios, { { "DIMM 1", "P0 CHANNEL A" } }, { 16384 });
        g_smbios.resize(g_smbios.size() - 10);   // the table's length says more than there is
        mods = hw::ReadMemoryModules();
        Check(mods.size() <= 1, "a table shorter than its length says: no read past the end");
    }

    SensorThreadPasses();
    WantPasses();
    VramPasses();
    PowerPasses();
    return Summary("system_sampler_test");
}
