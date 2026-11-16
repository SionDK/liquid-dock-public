#pragma once
// What the "PC 상태" panel shows beyond the dock's cell, read by the sensor thread (sensors.cpp): the
// internal drives with a drive letter, their models and temperatures, the installed memory modules from
// the SMBIOS table, and the GPU's own memory (VRAM). No administrator rights, no WMI. The rules without
// Windows are in sensor_logic.h.
#include <windows.h>
#include <winioctl.h>
#include <dxgi.h>
#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include "hw_sample.h"
#include "sensor_logic.h"

namespace hw {

// What a disk says about itself (StorageDeviceProperty: answered by the storage stack, the drive is not asked).
struct DiskInfo {
    bool ok = false;
    STORAGE_BUS_TYPE bus = BusTypeUnknown;
    bool removable = false;
    std::wstring vendor, product;      // as the drive reports them, trimmed
};

inline std::wstring DescriptorText(const BYTE* buf, DWORD bytes, DWORD offset) {
    std::wstring s;
    if (!offset || offset >= bytes) return s;
    for (DWORD i = offset; i < bytes && buf[i]; ++i) s += (wchar_t)buf[i];   // ASCII
    return sensorlogic::Trim(s);
}

inline DiskInfo QueryDisk(HANDLE h) {
    DiskInfo di;
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {}; DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &br, nullptr) ||
        br < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawPropertiesLength))
        return di;
    const auto* d = (const STORAGE_DEVICE_DESCRIPTOR*)buf;
    di.ok = true;
    di.bus = d->BusType;
    di.removable = d->RemovableMedia != FALSE;
    di.vendor = DescriptorText(buf, br, d->VendorIdOffset);
    di.product = DescriptorText(buf, br, d->ProductIdOffset);
    return di;
}

// Drives inside the PC: NVMe, SATA/ATA, SAS, SCSI, RAID. Not USB, SD/MMC, FireWire, a VHD (file-backed or
// virtual), a Storage Spaces disk (the pool's own disks have no letter), iSCSI or other network storage.
inline bool InternalBus(STORAGE_BUS_TYPE b) {
    switch (b) {
    case BusTypeNvme: case BusTypeSata: case BusTypeAta: case BusTypeSas: case BusTypeScsi: case BusTypeRAID: return true;
    default: return false;
    }
}

// The physical disk a drive letter lives on, -1 when none can be told. A volume on several disks (a
// spanned or striped dynamic volume): the disk of its first extent.
inline int VolumeDisk(wchar_t letter) {
    wchar_t path[] = L"\\\\.\\C:"; path[4] = letter;
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int disk = -1;
    STORAGE_DEVICE_NUMBER n = {}; DWORD br = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &n, sizeof(n), &br, nullptr) && n.DeviceType == FILE_DEVICE_DISK)
        disk = (int)n.DeviceNumber;
    else {
        union { VOLUME_DISK_EXTENTS v; BYTE room[sizeof(VOLUME_DISK_EXTENTS) + 7 * sizeof(DISK_EXTENT)]; } ext = {};
        // more extents than room: ERROR_MORE_DATA, with the first one filled in
        if ((DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &ext, sizeof(ext), &br, nullptr) || GetLastError() == ERROR_MORE_DATA) &&
            ext.v.NumberOfDiskExtents > 0)
            disk = (int)ext.v.Extents[0].DiskNumber;
    }
    CloseHandle(h);
    return disk;
}

// The panel's drives: every internal drive with a fixed drive letter, one row per physical disk (its first
// letter), ordered by letter. Looked for when the panel opens and whenever the drive letters change while it
// is open (GetLogicalDrives, about a microsecond); the disks are held open while the panel is, for their
// temperatures, and let go when it closes. Which disk a letter is on and what each disk says about itself
// (its bus and model) are kept from one opening to the next while the drive letters stay the same: an
// opening then only opens the disks again (no volume or property query); a letter added or taken away (a
// USB stick, a drive mounted) has everything asked again.
class DriveList {
public:
    ~DriveList() { Close(); }
    DriveList() = default;
    DriveList(const DriveList&) = delete;
    DriveList& operator=(const DriveList&) = delete;

    // force: look again even if the letters did not change (the panel opened). True when it looked.
    bool Refresh(int systemDisk, bool force) {
        const DWORD letters = GetLogicalDrives();
        if (!force && looked && letters == mask) return false;
        Close();
        mask = letters;
        looked = true;
        if (letters != knownMask) { known.clear(); knownMask = letters; }
        std::vector<int> seen;
        for (int i = 0; i < 26; ++i) {
            if (!(letters & (1u << i))) continue;
            const wchar_t letter = wchar_t(L'A' + i);
            Known* k = Find(letter);
            if (!k) {
                Known n;
                n.letter = letter;
                wchar_t root[] = L"C:\\"; root[0] = letter;
                n.disk = GetDriveTypeW(root) == DRIVE_FIXED ? VolumeDisk(letter) : -1;
                known.push_back(n);
                k = &known.back();
            }
            if (k->disk < 0 || std::find(seen.begin(), seen.end(), k->disk) != seen.end()) continue;
            seen.push_back(k->disk);
            if (k->asked && !k->internal) continue;          // USB, a VHD...: not opened again
            HANDLE h = OpenDisk(k->disk);
            if (h == INVALID_HANDLE_VALUE) continue;
            if (!k->asked) {
                const DiskInfo di = QueryDisk(h);
                if (di.ok) {   // asked again next time when the disk did not answer
                    k->asked = true;
                    k->internal = !di.removable && InternalBus(di.bus);
                    k->model = sensorlogic::CleanDriveModel(di.vendor, di.product);
                }
                if (!di.ok || !k->internal) { CloseHandle(h); continue; }
            }
            sensorlogic::DriveRow r;
            r.letter = letter;
            r.disk = k->disk;
            r.system = k->disk == systemDisk;
            r.model = k->model;
            rows.push_back(r);
            handles.push_back(h);
        }
        return true;
    }
    // every drive's temperature (NAN when it gives none)
    void ReadTemps() {
        for (size_t i = 0; i < rows.size(); ++i) {
            float t = 0;
            rows[i].temp = DiskTemp(handles[i], t) ? t : NAN;
        }
    }
    void Close() {
        for (HANDLE h : handles) CloseHandle(h);
        handles.clear();
        rows.clear();
        looked = false;
    }
    const std::vector<sensorlogic::DriveRow>& Rows() const { return rows; }

private:
    // a drive letter as last looked at: its disk (-1: not a fixed drive, or no disk to tell), and once that
    // disk answered StorageDeviceProperty whether it is an internal one and its cleaned model
    struct Known { wchar_t letter = 0; int disk = -1; bool asked = false, internal = false; std::wstring model; };
    Known* Find(wchar_t letter) {
        for (Known& k : known) if (k.letter == letter) return &k;
        return nullptr;
    }
    std::vector<sensorlogic::DriveRow> rows;
    std::vector<HANDLE> handles;          // rows[i]'s disk
    DWORD mask = 0;
    bool looked = false;
    std::vector<Known> known;             // kept across Close while the letters are knownMask
    DWORD knownMask = 0;
};

// How many rows DriveList would list now: a dock window's room for them before the panel first sampled
// (SensorClient::PanelRoomHint). The same rules (an internal bus, one per disk), the disks opened only for
// StorageDeviceProperty, which the storage stack answers (no drive is woken). UI thread, once; under 1 ms.
inline int CountPanelDrives() {
    DriveList l;
    l.Refresh(-1, true);
    return (int)l.Rows().size();
}

// The memory slots of the SMBIOS table (type 17), read once: they change only with a reboot. 0.03 - 0.2 ms.
// Empty when the table cannot be read.
inline std::vector<sensorlogic::MemoryModule> ReadMemoryModules() {
    const DWORD rsmb = 'RSMB';
    const UINT bytes = GetSystemFirmwareTable(rsmb, 0, nullptr, 0);
    if (bytes < 8) return {};
    std::vector<BYTE> buf(bytes);
    if (GetSystemFirmwareTable(rsmb, 0, buf.data(), bytes) != bytes) return {};
    // RawSMBIOSData: four version bytes, the table's length, the table
    DWORD len = 0;
    memcpy(&len, buf.data() + 4, sizeof(len));
    if (len > bytes - 8) len = bytes - 8;
    return sensorlogic::ParseSmbiosMemory(buf.data() + 8, len);
}
// ...read once for the process: the sensor thread and PanelRoomHint (UI thread) share it.
inline const std::vector<sensorlogic::MemoryModule>& MemoryModules() {
    static const std::vector<sensorlogic::MemoryModule> table = ReadMemoryModules();
    return table;
}

// The display adapters, as far as the panel's VRAM block needs them: whether NVML will answer (an NVIDIA
// adapter), and the memory of the adapter the dock draws with for a GPU NVML does not know.
struct VideoInfo {
    bool nvidia = false;       // an NVIDIA adapter is among them
    float dedicatedGB = 0;     // the first adapter's dedicated memory (DXGI_ADAPTER_DESC1::DedicatedVideoMemory)
    std::wstring name;         // ...and its name, "(R)" and "(TM)" dropped
};
// DXGI's adapter list. The first adapter is the one with the primary monitor's output (EnumAdapters1's
// order), which is the adapter the dock draws with (gfx.cpp: AdapterForPrimaryMonitor). dxgi.dll is loaded
// by name, as nvml.dll is (the dock has it loaded already; the tests that fake LoadLibraryExW get no
// adapters). No adapter is woken: descriptions only.
inline VideoInfo ReadVideo() {
    VideoInfo v;
    typedef HRESULT (WINAPI *create_t)(REFIID, void**);
    HMODULE lib = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create = lib ? (create_t)GetProcAddress(lib, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory1* f = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), (void**)&f)) || !f) return v;
    IDXGIAdapter1* a = nullptr;
    bool first = true;
    for (UINT i = 0; SUCCEEDED(f->EnumAdapters1(i, &a)) && a; ++i) {   // DXGI_ERROR_NOT_FOUND past the last
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {   // not the Basic Render Driver
            if (d.VendorId == 0x10DE) v.nvidia = true;
            if (first) {
                first = false;
                v.dedicatedGB = (float)(d.DedicatedVideoMemory / 1073741824.0);
                v.name = d.Description;
                for (const wchar_t* junk : { L"(R)", L"(TM)" }) { size_t p; while ((p = v.name.find(junk)) != std::wstring::npos) v.name.erase(p, wcslen(junk)); }
            }
        }
        a->Release();
    }
    f->Release();
    return v;
}
// ...asked once for the process (6 - 8 ms in a process without Direct3D, about 0.2 ms in the dock, which has
// its own factory by then): the sensor thread and PanelRoomHint (UI thread) share it - whichever comes
// second waits for the first.
inline const VideoInfo& Video() {
    static const VideoInfo v = ReadVideo();
    return v;
}

// The dock's sensor thread (SensorClient::Start, sensors.cpp), one pass at a time, with HWiNFO's reader a
// parameter (hwinfo::Reader there; a fake in tests/system_sampler_test.cpp):
//   WaitMs       1 s before the first pass (the cell filled soon after the dock starts), then every 2 s, every
//                second while a panel is open (SensorClient::SetPanelOpen also wakes it then);
//   SampleCell   what the dock's cell and the overheat alert need, always: hw::Sampler (CPU and memory load,
//                the NVIDIA GPU or PDH's GPU load, the system drive's temperature every 5 s by the clock) and
//                HWiNFO's CPU temperature - its memory and GPU memory readings too while a panel is open;
//   SamplePanel  while a panel is open only: the drive list (looked for when it opens, and when the letters
//                change) and every drive's temperature, the memory rows, the GPU's memory (NVML's amounts of
//                this pass, its memory temperature where it has one, otherwise HWiNFO's); the disks let go
//                when it closes.
// What the sensor settings leave out (Want, set before each pass from the UI thread's settings): HWiNFO not read
// at all (no registry or process polling) unless it is the source; the drives' temperatures not asked for (the
// system drive's every 5 s, the panel's every second) while the drives are hidden - their disks not held either.
// SampleCell comes first and is published before SamplePanel runs: a drive slow to answer its temperature
// (a sleeping disk spinning up) holds up the panel only, not the cell. The system drive's reading in the list
// stands for the sampler's own (SetSystemDriveTemp: asked once a second, not twice).
template <class HWiNFOReader>
class SensorThread {
public:
    explicit SensorThread(HWiNFOReader& reader) : hwi(reader) {}
    void Init() { Init(Video()); }
    void Init(const VideoInfo& adapters) { s.Init(); video = adapters; }   // a test's own adapters
    unsigned long WaitMs(bool panelOpen) const { return first || panelOpen ? 1000 : 2000; }
    void Want(bool hwinfo, bool driveTemps) { readHWiNFO = hwinfo; readDrives = driveTemps; s.SetDriveTemps(driveTemps); }
    void SampleCell(bool panelOpen, unsigned long long nowMs, LDSensorBlock& b) {
        first = false;
        s.Sample(b, nowMs);
        // NVML's, for the panel's VRAM block: the size once known stays (it does not change), so a pass NVML
        // does not answer (b starts empty) neither hides the block nor shows the adapter's size; its amount
        // in use is unknown for that pass
        if (b.gpuMemTotal > 0) { vramTotal = b.gpuMemTotal; vramUsed = b.gpuMemUsed; }
        else vramUsed = NAN;
        if (readHWiNFO) h = hwi.Read(panelOpen);
        else h = {};   // not the source: nothing of it (no CPU, memory or GPU memory reading)
        if (h.running) b.flags |= LD_HWINFO;
        if (h.cpuFound) { b.cpuTemp = h.cpuCelsius; b.flags |= LD_HAS_CPU_TEMP | LD_CPU_TEMP_HWINFO; }
    }
    // false (p untouched) while no panel is open
    bool SamplePanel(bool panelOpen, unsigned long long nowMs, sensorlogic::PanelData& p) {
        if (!panelOpen) {
            if (wasOpen) drives.Close();   // the disks are not held while no panel shows them
            wasOpen = false;
            return false;
        }
        if (readDrives) {
            drives.Refresh(s.SystemDisk(), !wasOpen);   // looked for when it opens, and when the letters change
            drives.ReadTemps();
            for (const auto& r : drives.Rows())
                if (r.system) s.SetSystemDriveTemp(!std::isnan(r.temp), r.temp, nowMs);   // not asked twice
            p.drives = drives.Rows();
        } else drives.Close();   // hidden: no disk held or asked (on again: looked for at once - Close forgets the look)
        wasOpen = true;
        p.drivesWithTemp = sensorlogic::CountWithTemp(p.drives);
        std::vector<sensorlogic::DimmTemp> temps;
        for (const auto& dp : h.dimms) {   // SampleCell's read asked for them (the panel open)
            sensorlogic::DimmTemp t;
            t.slot = dp.slot; t.locator = dp.locator; t.celsius = dp.celsius;
            temps.push_back(t);
        }
        const auto& slots = MemoryModules();
        p.dimms = sensorlogic::BuildDimmRows(slots, temps);
        p.modules = sensorlogic::InstalledModules(slots);
        p.dimmsWithTemp = sensorlogic::CountWithTemp(p.dimms);
        // the GPU's own memory: NVML's amounts (SampleCell just read them), its temperature from NVML where
        // the GPU gives one, otherwise from HWiNFO; without NVML the adapter's size and HWiNFO's readings
        sensorlogic::VramInput vi;
        vi.nvml = s.HasNvml();
        vi.nvmlUsedGB = vramUsed; vi.nvmlTotalGB = vramTotal;
        float memC = 0;
        if (s.GpuMemoryTemp(memC)) vi.nvmlTemp = memC;
        vi.adapterGB = video.dedicatedGB;
        vi.gpuName = s.HasNvml() ? s.GpuName() : video.name;
        for (const auto& g : h.gpus) {   // SampleCell's read asked for them (the panel open)
            sensorlogic::GpuMemoryReading r;
            r.gpu = g.gpu; r.sensor = g.sensor;
            if (g.hasTemp) r.celsius = g.celsius;
            if (g.hasUsed) r.usedMB = g.usedMB;
            if (g.hasUsage) r.usagePct = g.usagePct;
            vi.hwinfo.push_back(r);
        }
        p.vram = sensorlogic::BuildVram(vi);
        p.sampled = true;
        return true;
    }
    Sampler& Cell() { return s; }

private:
    HWiNFOReader& hwi;
    Sampler s;
    DriveList drives;
    decltype(std::declval<HWiNFOReader&>().Read(false)) h{};   // the last read
    VideoInfo video;
    float vramUsed = NAN, vramTotal = 0;   // GB: NVML's amount in use at the last SampleCell (NAN: no answer), its size (0: never known)
    bool first = true, wasOpen = false;
    bool readHWiNFO = true, readDrives = true;   // Want
};

}   // namespace hw
