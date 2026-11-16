#pragma once
// Hardware readings that need no administrator rights: CPU/RAM load, NVIDIA GPU (NVML, temperature
// included), GPU load for other GPUs (PDH) and - when the drive allows it - the system drive's
// temperature. Shared by the dock and the elevated helper. (The panel's drive list and memory slots:
// hw_panel.h.)
#include <windows.h>
#include <winioctl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstddef>
#include <string>
#include <vector>
#include <algorithm>
#include <cwchar>
#include "sensor_shared.h"
#include "sensor_logic.h"

namespace hw {

struct Nvml {
    typedef int (*init_t)();
    typedef int (*handle_t)(unsigned, void**);
    typedef int (*temp_t)(void*, int, unsigned*);
    struct Util { unsigned gpu, memory; };
    typedef int (*util_t)(void*, Util*);
    struct Mem { unsigned long long total, free, used; };
    typedef int (*mem_t)(void*, Mem*);
    typedef int (*name_t)(void*, char*, unsigned);
    // nvmlFieldValue_t (nvml.h), one field of nvmlDeviceGetFieldValues
    struct Field {
        unsigned fieldId, scopeId;
        long long timestamp, latencyUsec;
        int valueType, nvmlReturn;   // nvmlValueType_t; this field's own return code (0: its value is set)
        union { double d; int si; unsigned ui; unsigned long ul; unsigned long long ull; long long sll; unsigned short us; } value;
    };
    typedef int (*fields_t)(void*, int, Field*);
    temp_t temp = nullptr; util_t util = nullptr; mem_t mem = nullptr;
    fields_t fields = nullptr;
    HMODULE lib = nullptr;
    void* dev = nullptr;
    std::wstring name;
    int memTemp = 0;   // the memory's temperature: 0 not asked yet, 1 the GPU reports it, -1 it does not (not asked again)

    bool Init() {
        // System32 or NVIDIA's own folder only - never the application directory / search path
        HMODULE m = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) { wchar_t p[MAX_PATH]; ExpandEnvironmentStringsW(L"%ProgramFiles%\\NVIDIA Corporation\\NVSMI\\nvml.dll", p, MAX_PATH); m = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH); }
        if (!m) return false;
        lib = m;
        auto init = (init_t)GetProcAddress(m, "nvmlInit_v2");
        auto hnd = (handle_t)GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
        auto nm = (name_t)GetProcAddress(m, "nvmlDeviceGetName");
        temp = (temp_t)GetProcAddress(m, "nvmlDeviceGetTemperature");
        util = (util_t)GetProcAddress(m, "nvmlDeviceGetUtilizationRates");
        mem = (mem_t)GetProcAddress(m, "nvmlDeviceGetMemoryInfo");
        if (!init || !hnd || !temp || !util || !mem) return false;
        if (init() != 0 || hnd(0, &dev) != 0) return false;
        char n[96] = {};
        if (nm && nm(dev, n, sizeof(n)) == 0) {
            wchar_t w[96]; MultiByteToWideChar(CP_UTF8, 0, n, -1, w, 96); name = w;
        }
        return true;
    }

    // The GPU memory's temperature (NVML_FI_DEV_MEMORY_TEMP, field 82 of nvml.h), °C. Few GPUs report it:
    // a GeForce card looked at does not (driver 610.88, 2026-11-16: the call succeeds, the
    // field's own code is NVML_ERROR_NOT_SUPPORTED) - HWiNFO's Gadget key gives it there (hwinfo.h). Asked
    // when the panel first wants it, not at Init (the elevated helper shares this file and never asks); a
    // first answer that is no temperature ends the asking for good, later ones are only left out.
    bool MemoryTemp(float& celsius) {
        if (memTemp < 0 || !dev) return false;
        if (!fields && lib) fields = (fields_t)GetProcAddress(lib, "nvmlDeviceGetFieldValues");
        Field f = {};
        f.fieldId = 82;
        double v = -1;
        if (fields && fields(dev, 1, &f) == 0 && f.nvmlReturn == 0) {
            switch (f.valueType) {   // nvmlValueType_t
            case 0: v = f.value.d; break;
            case 1: v = f.value.ui; break;
            case 2: v = f.value.ul; break;
            case 3: v = (double)f.value.ull; break;
            case 4: v = (double)f.value.sll; break;
            case 5: v = f.value.si; break;
            case 6: v = f.value.us; break;
            }
        }
        if (!(v > 0 && v < 150)) {
            if (memTemp == 0) memTemp = -1;
            return false;
        }
        memTemp = 1;
        celsius = (float)v;
        return true;
    }
};
static_assert(sizeof(Nvml::Field) == 40, "nvmlFieldValue_t is 40 bytes (nvml.h)");

inline std::wstring CpuName() {
    std::wstring n = L"CPU";
    wchar_t buf[128]; DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
        n = buf;
        for (const wchar_t* junk : { L"AMD ", L"Intel(R) ", L"Core(TM) ", L"(R)", L"(TM)", L" Processor", L" CPU",
                                     L" 4-Core", L" 6-Core", L" 8-Core", L" 12-Core", L" 16-Core", L" 24-Core" }) {
            size_t p; while ((p = n.find(junk)) != std::wstring::npos) n.erase(p, wcslen(junk));
        }
        size_t at = n.find(L" @"); if (at != std::wstring::npos) n.erase(at);
        while (!n.empty() && n.back() == L' ') n.pop_back();
        while (!n.empty() && n.front() == L' ') n.erase(0, 1);
    }
    return n;
}

inline int SystemDiskNumber() {
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
    wchar_t vol[] = L"\\\\.\\C:"; vol[4] = win[0];
    HANDLE h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    STORAGE_DEVICE_NUMBER n = {}; DWORD br = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &n, sizeof(n), &br, nullptr);
    CloseHandle(h);
    return ok ? (int)n.DeviceNumber : 0;
}

inline HANDLE OpenDisk(int n) {
    wchar_t p[64]; swprintf_s(p, L"\\\\.\\PhysicalDrive%d", n);
    return CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

// A drive's temperature (StorageDeviceTemperatureProperty: no admin rights needed). The answer is a
// STORAGE_TEMPERATURE_DATA_DESCRIPTOR whose first STORAGE_TEMPERATURE_INFO is the drive's own (NVMe: its
// composite temperature), in whole °C. It goes to the drive (an NVMe drive leaves its low-power state for
// it), so it is asked every 5 s for the system drive and every second for the panel's drives only while
// the panel is open.
inline bool DiskTemp(HANDLE h, float& t) {
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceTemperatureProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {}; DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &br, nullptr)) return false;
    auto* d = (STORAGE_TEMPERATURE_DATA_DESCRIPTOR*)buf;
    if (br < offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo) + sizeof(STORAGE_TEMPERATURE_INFO) || d->InfoCount == 0) return false;
    const float v = (float)d->TemperatureInfo[0].Temperature;
    if (!(v > -40 && v < 150)) return false;
    t = v;
    return true;
}

class Sampler {
public:
    void Init() {
        haveNv = nv.Init();
        cpuName = CpuName();
        diskNo = SystemDiskNumber();
        disk = OpenDisk(diskNo);
        // the GPU's load from PDH where NVML has no answer (not an NVIDIA GPU): the dock's cell shows it
        if (!haveNv && PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &gpu3d) != ERROR_SUCCESS) gpu3d = nullptr;
            PdhCollectQueryData(q);
        }
        GetSystemTimes(&pi, &pk, &pu);
    }
    int SystemDisk() const { return diskNo; }
    // The panel's VRAM block (hw_panel.h): whether NVML answers for the GPU, its name ("" without NVML), and
    // its memory's temperature where NVML has one (asked by the panel's sample only).
    bool HasNvml() const { return haveNv; }
    const std::wstring& GpuName() const { return nv.name; }
    bool GpuMemoryTemp(float& celsius) { return haveNv && nv.MemoryTemp(celsius); }

    // The system drive's temperature read elsewhere (the panel's drive list holds that drive too and reads it
    // every second while the panel is open): taken as this period's reading, not asked again.
    void SetSystemDriveTemp(bool have, float celsius, unsigned long long nowMs) {
        haveSsdTemp = have;
        if (have) ssdTemp = celsius;
        tempPeriod.Mark(nowMs, kTempPeriodMs);
    }

    // Fills everything except the CPU temperature: CPU and memory load, the NVIDIA GPU (NVML) or any GPU's
    // load (PDH), the system drive's temperature (every 5 s by the clock, the last answer standing in
    // between). nowMs: GetTickCount64().
    void Sample(LDSensorBlock& s, unsigned long long nowMs) {
        FILETIME ci, ck, cu; GetSystemTimes(&ci, &ck, &cu);
        uint64_t di = U(ci) - U(pi), dk = U(ck) - U(pk), du = U(cu) - U(pu);
        pi = ci; pk = ck; pu = cu;
        uint64_t tot = dk + du;
        s.cpuLoad = tot ? 100.0f * float(tot - di) / float(tot) : 0;

        MEMORYSTATUSEX ms = { sizeof(ms) }; GlobalMemoryStatusEx(&ms);
        s.ramTotal = ms.ullTotalPhys / 1073741824.0f;
        s.ramUsed = (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0f;
        s.ramLoad = (float)ms.dwMemoryLoad;

        if (haveNv) {
            unsigned t = 0; Nvml::Util u = {}; Nvml::Mem m = {};
            if (nv.temp(nv.dev, 0, &t) == 0) { s.gpuTemp = (float)t; s.flags |= LD_HAS_GPU; }
            if (nv.util(nv.dev, &u) == 0) s.gpuLoad = (float)u.gpu;
            if (nv.mem(nv.dev, &m) == 0) { s.gpuMemUsed = m.used / 1073741824.0f; s.gpuMemTotal = m.total / 1073741824.0f; }
            s.flags |= LD_HAS_GPU_LOAD;
        }

        // The drive's temperature changes slowly: asked every 5 s by the clock (the sensor thread wakes every
        // 1 or 2 s), the last answer standing in between - a query every second keeps an NVMe drive out of its
        // low-power states.
        // Not asked at all while the drives are hidden (SetDriveTemps).
        if (driveTemps && disk != INVALID_HANDLE_VALUE && tempPeriod.Due(nowMs, kTempPeriodMs)) haveSsdTemp = DiskTemp(disk, ssdTemp);
        if (driveTemps && haveSsdTemp) { s.ssdTemp = ssdTemp; s.flags |= LD_HAS_SSD_TEMP; }

        // a rate counter sampled at every wake, 1 or 2 s apart: no long gap to discard
        if (q && gpu3d && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            double sum = 0;
            ForEach(gpu3d, [&](const wchar_t*, double v) { sum += v; });
            s.gpuLoad = (float)std::min(100.0, sum);
            s.flags |= LD_HAS_GPU_LOAD;
        }

        wcsncpy_s(s.cpuName, cpuName.c_str(), _TRUNCATE);
        wcsncpy_s(s.gpuName, nv.name.empty() ? L"GPU" : nv.name.c_str(), _TRUNCATE);
    }
    // The drives switched off in the sensor settings (the dock's sampler, hw_panel.h): the system drive's
    // temperature is neither asked for nor reported; on again, asked on its 5 s cadence.
    void SetDriveTemps(bool on) {
        if (!on) haveSsdTemp = false;
        driveTemps = on;
    }
    // the elevated helper's sample (sensors_main.cpp): its own clock
    void Sample(LDSensorBlock& s) { Sample(s, GetTickCount64()); }

    static constexpr unsigned long long kTempPeriodMs = 5000;

private:
    template <class F> void ForEach(PDH_HCOUNTER c, F f) {
        if (!c) return;
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA) return;
        std::vector<BYTE> buf(bytes);
        auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return;
        for (DWORD i = 0; i < count; ++i)
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                f(items[i].szName, items[i].FmtValue.doubleValue);
    }
    static uint64_t U(FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

    Nvml nv; bool haveNv = false;
    std::wstring cpuName;
    int diskNo = 0;
    HANDLE disk = INVALID_HANDLE_VALUE;
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER gpu3d = nullptr;
    FILETIME pi{}, pk{}, pu{};
    sensorlogic::Period tempPeriod;          // the system drive's temperature
    bool haveSsdTemp = false;
    float ssdTemp = 0;
    bool driveTemps = true;                  // SetDriveTemps
};

} // namespace hw
