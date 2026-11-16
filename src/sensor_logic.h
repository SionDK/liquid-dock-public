#pragma once
// The "PC 상태" panel's data rules that need no Windows: drive model names, memory slots from the SMBIOS
// table (type 17), which HWiNFO memory temperature belongs to which slot, the GPU's memory block (VRAM: when
// it shows, where its amounts and temperature come from), the panel-open linger and the time-based periods
// of the sensor thread, what the dock's sensor cell shows (its key and text), and what the sensor settings
// show (the source, the six items) with the panel's layout that follows from them. Tested by
// tests/sensor_logic_test.cpp (portable: g++/clang/cl).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace sensorlogic {

// ---- rows the panel shows (SensorClient::Panel(), sensors.h) -------------------------------------------

// One internal drive with a drive letter.
struct DriveRow {
    wchar_t letter = 0;        // L'C': the first (lowest) letter on that physical disk
    int disk = -1;             // physical disk number (\\.\PhysicalDriveN)
    bool system = false;       // the disk Windows runs from
    std::wstring model;        // cleaned model name ("WD SN850X"), never empty
    float temp = NAN;          // °C, NAN when the drive does not report one
};

// One memory module (or one HWiNFO memory temperature without a known module).
struct DimmRow {
    std::wstring label;        // "RAM 1 · A2", or "RAM 2" when the slot has no name
    std::wstring slot;         // "A2", empty when unknown
    float temp = NAN;          // °C, NAN when HWiNFO does not report it
};

// The GPU's own memory (VRAM): the panel's block between the memory rows and the drives (BuildVram below).
struct VramRow {
    bool present = false;      // the GPU has memory of its own of a known size; false: the block is left out
    float totalGB = 0;
    float usedGB = NAN;        // NAN: no source says how much is in use ("--")
    float load = NAN;          // % of it in use (0 - 100), NAN with usedGB
    float temp = NAN;          // °C, the memory's (junction) temperature; NAN: no source reports it ("--°C")
    bool nvmlTemp = false;     // ...and it is NVML's (the GPU gives one): the row stays without HWiNFO too
};

// The "PC 상태" panel's own readings, beyond the LDSensorBlock (SensorClient::Data()) the cell and the panel
// share. Sampled only while a panel is open (SensorClient::SetPanelOpen), once a second, the first sample
// right when it opens; between two openings the last sample stays (drives, slots and names do not change
// much). Kept by the dock (SensorClient::Panel(), sensors.h), not in the helper's shared memory.
struct PanelData {
    // Internal drives with a drive letter (NVMe/SATA/SAS/SCSI/RAID; not USB, SD, VHD, Storage Spaces), one
    // per physical disk (its lowest letter), ordered by letter. Each DriveRow:
    //   letter  L'C'                     disk    physical disk number
    //   system  the drive Windows runs from (its temperature is also Data().ssdTemp, the overheat alert's)
    //   model   cleaned model name, never empty: "WD SN850X", "Samsung 870 EVO", "Samsung MZVL21T0HCLR-00B00"
    //   temp    °C (whole degrees), NAN when the drive gives none ("--°C")
    std::vector<DriveRow> drives;
    // Memory modules, in the board's table order (two modules, say: "RAM 1 · A2", "RAM 2 · B2"). Each DimmRow:
    // label "RAM k · <slot>" or "RAM k" (ready to draw), slot ("A2", may be empty), temp °C from HWiNFO's
    // Gadget key (one decimal is meaningful), NAN when HWiNFO does not report it.
    // Empty: no modules known and no HWiNFO memory temperatures (soldered memory, a laptop): hide the rows.
    std::vector<DimmRow> dimms;
    // The GPU's memory: used / total and its load as the RAM line shows the PC's, and its temperature in a
    // row like a memory module's. present false (an integrated GPU without memory of its own, or its size
    // unknown): the block is not shown and the panel is as before.
    VramRow vram;
    int drivesWithTemp = 0;   // footer "SSD a/b": a = drivesWithTemp, b = drives.size()
    int modules = 0;          // footer "RAM a/b": b = installed modules (SMBIOS); 0: leave the RAM part out
    int dimmsWithTemp = 0;    //                    a = dimms with a temperature
    bool sampled = false;     // false until the first panel sample arrived (rows empty then: "측정 중…")
};

// ---- small text helpers ------------------------------------------------------------------------------------

inline wchar_t AsciiUpper(wchar_t c) { return (c >= L'a' && c <= L'z') ? wchar_t(c - L'a' + L'A') : c; }
inline wchar_t AsciiLower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? wchar_t(c - L'A' + L'a') : c; }
inline bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
inline bool IsLetter(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }

inline std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == 0)) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == 0)) --b;
    return s.substr(a, b - a);
}
inline std::wstring Upper(std::wstring s) { for (auto& c : s) c = AsciiUpper(c); return s; }
inline bool SameNoCase(const std::wstring& a, const std::wstring& b) { return Upper(a) == Upper(b); }
// lower case, no spaces: "P0 CHANNEL A/DIMM 1" and "p0 channel a / dimm 1" compare equal
inline std::wstring Squeeze(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) if (c != L' ' && c != L'\t') o += AsciiLower(c);
    return o;
}
inline std::vector<std::wstring> Words(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'\t') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
inline std::wstring Join(const std::vector<std::wstring>& w) {
    std::wstring o;
    for (const auto& x : w) { if (!o.empty()) o += L' '; o += x; }
    return o;
}

// ---- drive model names -------------------------------------------------------------------------------------

// "2000GB", "1TB", "500G", "1.92TB", "512gb": a capacity and nothing else. Not "1T0" (part of Samsung's model codes).
inline bool IsCapacityWord(const std::wstring& w) {
    size_t i = 0;
    while (i < w.size() && IsDigit(w[i])) ++i;
    if (i == 0) return false;
    if (i < w.size() && w[i] == L'.') {
        size_t j = i + 1;
        while (j < w.size() && IsDigit(w[j])) ++j;
        if (j == i + 1) return false;
        i = j;
    }
    const std::wstring unit = Upper(w.substr(i));
    return unit == L"G" || unit == L"GB" || unit == L"T" || unit == L"TB" || unit == L"M" || unit == L"MB";
}

// The model name the panel shows for a drive, from its vendor and product ids (StorageDeviceProperty):
//   '_' becomes a space; a standalone "SSD" word goes; trailing capacity words go ("2000GB", "1TB");
//   a vendor id that is a bus name ("NVMe", "ATA") or that the product id repeats goes;
//   WD's product lines ("WD_BLACK", "WD_BLUE", ...) and "WDC" become "WD";
//   an all-caps vendor word of 4+ letters is title-cased (SAMSUNG -> Samsung), brands written in capitals
//   stay (KIOXIA, ADATA); model codes are left as they are.
//   "WD_BLACK SN850X 2000GB" -> "WD SN850X", "Samsung SSD 870 EVO 1TB" -> "Samsung 870 EVO",
//   "SAMSUNG MZVL21T0HCLR-00B00" -> "Samsung MZVL21T0HCLR-00B00". Never empty ("Disk" at worst).
inline std::wstring CleanDriveModel(const std::wstring& vendorId, const std::wstring& productId) {
    std::wstring vendor = Trim(vendorId), product = Trim(productId);
    const std::wstring v = Upper(vendor);
    if (v == L"NVME" || v == L"ATA" || v == L"SATA" || v == L"SCSI" || v == L"USB" ||
        Upper(product).rfind(v, 0) == 0)
        vendor.clear();
    std::wstring all = vendor.empty() ? product : vendor + L" " + product;
    for (auto& c : all) if (c == L'_') c = L' ';
    std::vector<std::wstring> w = Words(all);
    std::vector<std::wstring> kept;
    for (auto& x : w) if (Upper(x) != L"SSD") kept.push_back(x);
    while (!kept.empty() && IsCapacityWord(kept.back())) kept.pop_back();
    if (!kept.empty() && Upper(kept[0]) == L"WDC") kept[0] = L"WD";
    if (kept.size() > 2 && Upper(kept[0]) == L"WD") {
        const std::wstring line = Upper(kept[1]);
        if (line == L"BLACK" || line == L"BLUE" || line == L"RED" || line == L"GREEN" || line == L"PURPLE")
            kept.erase(kept.begin() + 1);
    }
    if (!kept.empty()) {
        std::wstring& first = kept[0];
        bool caps = first.size() >= 4;
        for (wchar_t c : first) if (!(c >= L'A' && c <= L'Z')) caps = false;
        if (caps && first != L"ADATA" && first != L"KIOXIA")
            for (size_t i = 1; i < first.size(); ++i) first[i] = AsciiLower(first[i]);
    }
    std::wstring out = Join(kept);
    if (out.empty()) {   // the name was only "SSD" or a capacity: what the drive said, tidied
        for (auto& c : product) if (c == L'_') c = L' ';
        out = Join(Words(product));
    }
    return out.empty() ? L"Disk" : out;
}

// ---- memory modules: SMBIOS type 17 ------------------------------------------------------------------------

struct MemoryModule {
    std::wstring bank, device;       // Bank Locator ("P0 CHANNEL A"), Device Locator ("DIMM 1")
    unsigned formFactor = 0;         // 0x09 DIMM, 0x0D SODIMM, 0x11 CAMM, 0x0B row of chips, ...
    uint64_t sizeMB = 0;             // 0: empty slot
    bool installed = false;          // a module sits there: size known and not 0, a socketed form factor
};

// Every type 17 structure of a raw SMBIOS structure table (GetSystemFirmwareTable('RSMB') without its
// 8-byte header), in table order. A truncated or malformed table ends the walk: what was read stands.
inline std::vector<MemoryModule> ParseSmbiosMemory(const uint8_t* t, size_t len) {
    std::vector<MemoryModule> out;
    size_t p = 0;
    while (t && p + 4 <= len) {
        const uint8_t type = t[p], head = t[p + 1];
        if (head < 4 || p + head > len) break;
        // the strings after the formatted part: each ends with a NUL, the set with another (two NULs when none)
        std::vector<std::wstring> strs;
        size_t q = p + head;
        bool ended = false;
        if (q + 1 < len && t[q] == 0 && t[q + 1] == 0) { q += 2; ended = true; }
        else {
            while (q < len) {
                const size_t b = q;
                while (q < len && t[q]) ++q;
                if (q >= len) break;
                std::wstring s;
                for (size_t i = b; i < q; ++i) s += (wchar_t)t[i];
                strs.push_back(Trim(s));
                ++q;
                if (q < len && t[q] == 0) { ++q; ended = true; break; }
            }
        }
        if (type == 17 && head >= 0x13) {
            auto str = [&](uint8_t n) { return n >= 1 && (size_t)n <= strs.size() ? strs[n - 1] : std::wstring(); };
            MemoryModule m;
            const unsigned size = t[p + 0x0C] | (t[p + 0x0D] << 8);
            m.formFactor = t[p + 0x0E];
            m.device = str(t[p + 0x10]);
            m.bank = str(t[p + 0x11]);
            if (size == 0x7FFF && head >= 0x20)
                m.sizeMB = (uint32_t)(t[p + 0x1C] | (t[p + 0x1D] << 8) | (t[p + 0x1E] << 16) | ((uint32_t)t[p + 0x1F] << 24)) & 0x7FFFFFFFu;
            else if (size != 0xFFFF)
                m.sizeMB = (size & 0x8000) ? (size & 0x7FFF) / 1024 + ((size & 0x7FFF) ? 1 : 0) : size;
            m.installed = size != 0 && size != 0xFFFF &&
                          (m.formFactor == 0x09 || m.formFactor == 0x0D || m.formFactor == 0x11);
            out.push_back(m);
        }
        if (type == 127 || !ended) break;   // end of table, or strings running off its end
        p = q;
    }
    return out;
}

// The slot's name on the board, best effort; empty when the strings say nothing usable.
//   AMD AGESA:  Bank "P0 CHANNEL A" + Device "DIMM 1"         -> "A2" (DIMM k is the board's slot k+1)
//   Intel:      Device "Controller1-ChannelA-DIMM0"           -> "B1" (controller = the board's channel)
//               Device "ChannelB-DIMM1"                       -> "B2"
//   other:      "DIMM_A1" / "DDR5_B2" -> "A1" / "B2"; any other Device Locator of 8 characters at most as it is
inline std::wstring SlotName(const std::wstring& bankLocator, const std::wstring& deviceLocator) {
    const std::wstring bank = Upper(Trim(bankLocator)), dev = Upper(Trim(deviceLocator));
    auto number = [](const std::wstring& s, size_t& i, int& n) {
        size_t b = i; n = 0;
        while (i < s.size() && IsDigit(s[i]) && i - b < 4) n = n * 10 + (s[i++] - L'0');
        return i > b;
    };
    {   // AMD: "P<n> CHANNEL <X>" + "DIMM <k>"
        size_t i = 0; int n = 0, k = 0;
        if (bank.size() > 1 && bank[0] == L'P' && (i = 1, number(bank, i, n))) {
            const std::wstring rest = Trim(bank.substr(i));
            if (rest.size() == 9 && rest.compare(0, 8, L"CHANNEL ") == 0 && IsLetter(rest[8]) && dev.rfind(L"DIMM", 0) == 0) {
                size_t j = 4;
                while (j < dev.size() && dev[j] == L' ') ++j;
                if (number(dev, j, k) && j == dev.size()) return std::wstring(1, rest[8]) + std::to_wstring(k + 1);
            }
        }
    }
    {   // Intel: "[Controller<c>-]Channel<X>-DIMM<k>"
        size_t i = 0; int c = -1, k = 0;
        if (dev.rfind(L"CONTROLLER", 0) == 0) {
            i = 10;
            if (!number(dev, i, c) || i >= dev.size() || dev[i] != L'-') c = -2;
            else ++i;
        }
        if (c != -2 && dev.compare(i, 7, L"CHANNEL") == 0 && i + 8 < dev.size() && IsLetter(dev[i + 7]) && dev[i + 8] == L'-') {
            const wchar_t ch = dev[i + 7];
            size_t j = i + 9;
            if (dev.compare(j, 4, L"DIMM") == 0) {
                j += 4;
                if (number(dev, j, k) && j == dev.size()) {
                    const wchar_t letter = c >= 0 ? wchar_t(L'A' + c) : ch;
                    if (letter <= L'Z') return std::wstring(1, letter) + std::to_wstring(k + 1);
                }
            }
        }
    }
    {   // "DIMM_A1", "DDR5_B2", "DIMM-A1": the letter and number
        for (const wchar_t* pre : { L"DIMM_", L"DIMM-", L"DDR4_", L"DDR5_", L"DDR_" }) {
            const std::wstring p = pre;
            if (dev.rfind(p, 0) == 0 && dev.size() > p.size() + 1 && IsLetter(dev[p.size()])) {
                size_t j = p.size() + 1; int k = 0;
                if (number(dev, j, k) && j == dev.size()) return dev.substr(p.size());
            }
        }
    }
    const std::wstring d = Trim(deviceLocator);
    return d.size() <= 8 ? d : std::wstring();
}

// A HWiNFO DDR5 sensor's "(Bank/Device)" is that module's type 17 strings.
inline bool LocatorMatches(const std::wstring& locator, const MemoryModule& m) {
    return !locator.empty() && Squeeze(locator) == Squeeze(m.bank + L"/" + m.device);
}

// One memory temperature from HWiNFO (hwinfo::PickDimms), in HWiNFO's module order.
struct DimmTemp {
    int slot = 0;              // HWiNFO's module number ([#n], DIMM[n])
    std::wstring locator;      // "(Bank/Device)" of a DDR5 sensor's name, empty otherwise
    float celsius = NAN;
};

inline std::wstring DimmLabel(int k, const std::wstring& slot) {
    std::wstring s = L"RAM " + std::to_wstring(k);
    if (!slot.empty()) s += L" · " + slot;
    return s;
}

// The panel's memory rows from the installed modules (SMBIOS, all of type 17 may be passed: the empty
// slots are skipped) and HWiNFO's temperatures.
//  - every temperature matched to a module by its "(Bank/Device)": one row per installed module, in
//    table order, its temperature or NAN ("--°C": HWiNFO does not report it);
//  - otherwise one row per temperature in HWiNFO's order (the slot name only where it matched), then as
//    many unmatched modules without a temperature as make the rows as many as the modules;
//  - slot names two modules would share are not used (the strings were not understood).
inline std::vector<DimmRow> BuildDimmRows(const std::vector<MemoryModule>& table, const std::vector<DimmTemp>& temps) {
    std::vector<const MemoryModule*> mods;
    for (const auto& m : table) if (m.installed) mods.push_back(&m);
    std::vector<std::wstring> names;
    for (auto* m : mods) names.push_back(SlotName(m->bank, m->device));
    for (size_t i = 0; i < names.size(); ++i)
        for (size_t j = i + 1; j < names.size(); ++j)
            if (!names[i].empty() && SameNoCase(names[i], names[j])) { for (auto& n : names) n.clear(); i = names.size(); break; }

    std::vector<int> moduleOf(temps.size(), -1);
    std::vector<bool> taken(mods.size(), false);
    bool allMatched = true;
    for (size_t t = 0; t < temps.size(); ++t) {
        for (size_t m = 0; m < mods.size(); ++m)
            if (!taken[m] && LocatorMatches(temps[t].locator, *mods[m])) { taken[m] = true; moduleOf[t] = (int)m; break; }
        if (moduleOf[t] < 0) allMatched = false;
    }
    std::vector<DimmRow> rows;
    if (allMatched) {
        for (size_t m = 0; m < mods.size(); ++m) {
            DimmRow r;
            r.slot = names[m];
            for (size_t t = 0; t < temps.size(); ++t) if (moduleOf[t] == (int)m) r.temp = temps[t].celsius;
            rows.push_back(r);
        }
    } else {
        for (size_t t = 0; t < temps.size(); ++t) {
            DimmRow r;
            if (moduleOf[t] >= 0) r.slot = names[moduleOf[t]];
            r.temp = temps[t].celsius;
            rows.push_back(r);
        }
        for (size_t m = 0; m < mods.size() && rows.size() < mods.size(); ++m)
            if (!taken[m]) { DimmRow r; r.slot = names[m]; rows.push_back(r); }
    }
    for (size_t i = 0; i < rows.size(); ++i) rows[i].label = DimmLabel((int)i + 1, rows[i].slot);
    return rows;
}

inline int InstalledModules(const std::vector<MemoryModule>& table) {
    int n = 0;
    for (const auto& m : table) n += m.installed ? 1 : 0;
    return n;
}

template <class Rows> int CountWithTemp(const Rows& rows) {
    int n = 0;
    for (const auto& r : rows) n += std::isnan(r.temp) ? 0 : 1;
    return n;
}

// ---- the GPU's memory (VRAM) ---------------------------------------------------------------------------------

// What HWiNFO reports about one GPU's memory (hwinfo::PickGpuMemory), by HWiNFO's GPU number.
struct GpuMemoryReading {
    int gpu = 0;               // HWiNFO's GPU number ([#n])
    std::wstring sensor;       // the sensor's name, which holds the GPU's model:
                               // "dGPU [#0]: NVIDIA GeForce <model>: <board> (...)"
    float celsius = NAN;       // the memory's (junction) temperature
    float usedMB = NAN;        // dedicated memory in use
    float usagePct = NAN;      // ...as a share of the GPU's memory
};

// Everything known about the memory of the GPU the panel's card shows, for BuildVram.
struct VramInput {
    bool nvml = false;         // NVML answers for that GPU (NVIDIA): used and total come from it
    float nvmlUsedGB = 0, nvmlTotalGB = 0;   // used NAN: NVML did not answer this pass (the total is its last)
    float nvmlTemp = NAN;      // NVML's memory temperature (NVML_FI_DEV_MEMORY_TEMP): few GPUs give one
    float adapterGB = 0;       // without NVML: the dedicated memory of the adapter the dock draws with (DXGI)
    std::wstring gpuName;      // the card's GPU name ("NVIDIA GeForce RTX 5080"; "GPU" or empty: unknown)
    std::vector<GpuMemoryReading> hwinfo;
};

// Dedicated memory an adapter must report (DXGI) to count as a GPU with memory of its own when NVML does
// not answer for it: an integrated GPU reports none or a token amount (Intel: 128 MB; an AMD APU its UMA
// frame buffer, 512 MB by default - one set to 1 GB or more is memory of the GPU's own and is shown).
constexpr float kMinAdapterVramGB = 1.0f;

// Whether a dock window should keep room for the VRAM block before the panel first sampled
// (SensorClient::PanelRoomHint): an NVIDIA adapter is there (NVML will answer), or the adapter the dock
// draws with has memory of its own.
inline bool VramExpected(bool nvidiaAdapter, float adapterGB) {
    return nvidiaAdapter || adapterGB >= kMinAdapterVramGB;
}

// Whether a HWiNFO GPU sensor is the card's GPU: one of its name's parts between the colons ("dGPU [#0]",
// the model, the board) is the GPU's name - the whole part, so that an "RTX 4060" is not found in the sensor
// of an "RTX 4060 Ti". Case and spaces do not count.
inline bool GpuSensorNamed(const std::wstring& sensor, const std::wstring& gpuName) {
    const std::wstring name = Squeeze(Trim(gpuName));
    if (name.empty() || name == L"gpu") return false;
    const std::wstring s = Squeeze(sensor);
    for (size_t a = 0; a <= s.size();) {
        size_t b = s.find(L':', a);
        if (b == std::wstring::npos) b = s.size();
        if (s.compare(a, b - a, name) == 0) return true;
        a = b + 1;
    }
    return false;
}

// HWiNFO's amount in use is taken up to this many times the memory's size (the adapter's size from DXGI is a
// little under the real one); beyond it the value was not understood (another unit) and is left out.
constexpr float kMaxVramOverTotal = 1.5f;

// The panel's VRAM block.
//  - shown (present) when the memory's size is known: NVML's total, otherwise the adapter's dedicated memory
//    from kMinAdapterVramGB on; hidden otherwise (an integrated GPU: the panel as before);
//  - used: NVML's; without NVML (or in a pass it did not answer) what HWiNFO reports for the GPU - its
//    dedicated memory in use (MB, at most kMaxVramOverTotal times the size), or its usage in % of the total;
//    "--" when neither is there;
//  - temperature: NVML's when the GPU gives one, otherwise HWiNFO's memory (junction) temperature;
//  - of several GPUs in HWiNFO: those whose sensor name holds the card's GPU name, the lowest number first;
//    when no sensor is named so (the name unknown), the lowest-numbered GPU that has the reading.
inline VramRow BuildVram(const VramInput& in) {
    VramRow v;
    const bool nvml = in.nvml && in.nvmlTotalGB > 0;
    if (nvml) v.totalGB = in.nvmlTotalGB;
    else if (in.adapterGB >= kMinAdapterVramGB) v.totalGB = in.adapterGB;
    else return v;
    v.present = true;
    bool named = false;
    for (const auto& r : in.hwinfo) named = named || GpuSensorNamed(r.sensor, in.gpuName);
    auto pick = [&](float GpuMemoryReading::* field) {
        const GpuMemoryReading* best = nullptr;
        for (const auto& r : in.hwinfo) {
            if (std::isnan(r.*field) || (named && !GpuSensorNamed(r.sensor, in.gpuName))) continue;
            if (!best || r.gpu < best->gpu) best = &r;
        }
        return best ? best->*field : NAN;
    };
    if (nvml && !std::isnan(in.nvmlUsedGB)) v.usedGB = in.nvmlUsedGB;
    else {
        const float usedMB = pick(&GpuMemoryReading::usedMB), pct = pick(&GpuMemoryReading::usagePct);
        if (!std::isnan(usedMB) && usedMB / 1024.0f <= kMaxVramOverTotal * v.totalGB) v.usedGB = usedMB / 1024.0f;
        else if (!std::isnan(pct)) v.usedGB = v.totalGB * pct / 100.0f;
    }
    if (!std::isnan(v.usedGB)) {
        if (v.usedGB < 0) v.usedGB = 0;
        const float load = 100.0f * v.usedGB / v.totalGB;
        v.load = load > 100.0f ? 100.0f : load;
    }
    v.nvmlTemp = !std::isnan(in.nvmlTemp);
    v.temp = v.nvmlTemp ? in.nvmlTemp : pick(&GpuMemoryReading::celsius);
    return v;
}

// 0 normal, 1 warm (within `band` °C of hot), 2 hot; unknown is normal.
inline int Severity(float celsius, float hot, float band = 10.f) {
    if (std::isnan(celsius)) return 0;
    return celsius >= hot ? 2 : (celsius >= hot - band ? 1 : 0);
}

// What the dock's sensor cell shows for one metric (CPU, GPU): its temperature when known ("47°", in the
// colour of its severity), otherwise its load ("12%", normal colour), otherwise "--". Rounded as drawn
// (lround). The dock redraws for a sensor sample only when the cell's key changes (sensors.h, cellVersion):
// DrawSensorCell draws from these, so what it draws cannot drift from the key.
struct CellReading {
    int8_t kind = 0;   // 0 none ("--"), 1 temperature ("N°"), 2 load ("N%")
    int8_t sev = 0;    // Severity of the temperature (0 for a load or none)
    long value = 0;    // the rounded number
    bool operator==(const CellReading&) const = default;
};
struct CellKey {
    CellReading cpu, gpu;
    bool live = false;
    // false: the GPU is hidden (sensor settings, ShowItems::gpu) - gpu is then none (kind 0) and the cell
    // shows the CPU's line alone, centred. A GPU that is shown but reports nothing keeps its "GPU --" line.
    bool gpuLine = true;
    bool operator==(const CellKey&) const = default;
};
// temp/load NAN when unknown; hot: the metric's threshold (warm 10 °C below, as Severity)
inline CellReading MakeCellReading(float temp, float load, float hot) {
    CellReading r;
    if (!std::isnan(temp)) { r.kind = 1; r.value = std::lround(temp); r.sev = (int8_t)Severity(temp, hot); }
    else if (!std::isnan(load)) { r.kind = 2; r.value = std::lround(load); }
    return r;
}
// the cell's text for a reading: "47°", "12%", "--" (the label "CPU " / "GPU " is the cell's)
inline std::wstring CellText(const CellReading& r) {
    if (r.kind == 1) return std::to_wstring((int)r.value) + L"°";
    if (r.kind == 2) return std::to_wstring((int)r.value) + L"%";
    return L"--";
}

// ---- the sensor settings (the settings window's 센서 page, settings.ini) ----------------------------------

// Where every HWiNFO / PawnIO value is read from (settings sensorSource): one source at a time.
//   off     neither: no HWiNFO registry or process polling, no helper asked for sensors
//   HWiNFO  HWiNFO's Gadget key (CPU temperature, memory module temperatures, VRAM used and temperature)
//   PawnIO  the elevated helper's CPU temperature (PawnIO driver); HWiNFO is not read
// What needs neither stays in every mode: CPU / GPU load, RAM, NVML's GPU temperature, VRAM amounts and
// VRAM temperature, DXGI's VRAM size, the drives' temperatures (IOCTL), names, the SMBIOS module count.
enum : int { kSourceOff = 0, kSourceHWiNFO = 1, kSourcePawnIO = 2 };

// A settings file from before sensorSource with "CPU 온도 직접 읽기 (PawnIO)" on (cpuTemp=1): that dock read
// HWiNFO always and took the helper's CPU temperature only when HWiNFO gave none. Its first start with the
// source keeps what it showed: PawnIO only if the PawnIO service is there and HWiNFO reports no CPU
// temperature; otherwise HWiNFO (PawnIO removed since, or HWiNFO's value was the one on screen).
inline int MigratedPawnIOSource(bool pawnioInstalled, bool hwinfoCpuTemp) {
    return pawnioInstalled && !hwinfoCpuTemp ? kSourcePawnIO : kSourceHWiNFO;
}

// The source and the six "PC 상태에 보일 항목" switches, as the cell, the panel, the overheat alert and the
// peek use them (SensorClient::Items(): read from the settings on the UI thread). A value whose only source
// is not active in the current mode is hidden like a switched-off one, not shown as "--".
struct ShowItems {
    int source = kSourceHWiNFO;
    bool cpuTemp = true, gpu = true, vram = true, vramTemp = true, ramTemp = true, ssd = true;
    bool ReadsHWiNFO() const { return source == kSourceHWiNFO; }
    // the helper is asked for sensors (its want event: it then holds the PawnIO driver)
    bool WantsPawnIO() const { return source == kSourcePawnIO && cpuTemp; }
    // the CPU's temperature: the card's big number, the cell (otherwise its load), the alert and the peek
    bool CpuTempShown() const { return cpuTemp && source != kSourceOff; }
    // the memory module rows: their temperatures come from HWiNFO only
    bool DimmsShown() const { return ramTemp && source == kSourceHWiNFO; }
    // the VRAM block's temperature row: HWiNFO's, or NVML's on the GPUs that give one (nvmlTempSeen)
    bool VramTempShown(bool nvmlTempSeen) const { return vram && vramTemp && (source == kSourceHWiNFO || nvmlTempSeen); }
};

// ---- the panel's layout (DIP at scale 1; Dock::DrawPanel in dock_sensors.cpp draws it) ---------------------

// Header 0 .. 55, the CPU (and GPU) card 55 .. 139, the RAM line 139 .. 183 - always there; then the memory
// module rows, the VRAM block, a divider, the drive rows and the footer.
constexpr float kPanelListTop = 183, kPanelRowH = 27, kPanelVramLineH = 44, kPanelDividerRoom = 8, kPanelFooterH = 33;
// the VRAM block: 0 none, 1 its line and bar (44 DIP), 2 and its temperature row (71)
inline float PanelVramDip(int vram) { return vram <= 0 ? 0 : vram == 1 ? kPanelVramLineH : kPanelVramLineH + kPanelRowH; }
// the divider: only above drive rows, and only when module rows or the VRAM block are above them
inline bool PanelDivider(int dimmRows, int vram, int driveRows) { return driveRows > 0 && (dimmRows > 0 || vram > 0); }
inline float PanelHeightDip(int dimmRows, int driveRows, int vram) {
    return kPanelListTop + dimmRows * kPanelRowH + PanelVramDip(vram) + (PanelDivider(dimmRows, vram, driveRows) ? kPanelDividerRoom : 0) +
           driveRows * kPanelRowH + kPanelFooterH;
}

// What the panel names at its top right: "CPU · GPU · RAM · VRAM · SSD", without what it does not show.
inline std::wstring PanelHeader(bool gpu, bool vram, bool ssd) {
    std::wstring s = L"CPU";
    if (gpu) s += L" · GPU";
    s += L" · RAM";
    if (vram) s += L" · VRAM";
    if (ssd) s += L" · SSD";
    return s;
}

// The footer: on the left how many drives and modules report a temperature and the pace ("SSD 3/3 · RAM 2/2 ·
// 1초 갱신"), on the right what was left out for want of room ("SSD 외 2개 · RAM 외 1개"). A hidden item (SSD or
// the module rows switched off, or the modules without HWiNFO) has no part in either.
struct PanelFooterText { std::wstring counts, leftOut; };
inline PanelFooterText PanelFooter(bool sampled, bool drivesShown, int drives, int drivesWithTemp, int drivesHidden,
                                   bool dimmsShown, int modules, int dimmsWithTemp, int dimmsHidden) {
    PanelFooterText f;
    if (!sampled) { f.counts = L"측정 중…"; return f; }
    if (drivesShown && drives > 0) f.counts += L"SSD " + std::to_wstring(drivesWithTemp) + L"/" + std::to_wstring(drives) + L" · ";
    if (dimmsShown && modules > 0) f.counts += L"RAM " + std::to_wstring(dimmsWithTemp) + L"/" + std::to_wstring(modules) + L" · ";
    f.counts += L"1초 갱신";
    if (drivesShown && drivesHidden > 0) f.leftOut += L"SSD 외 " + std::to_wstring(drivesHidden) + L"개";
    if (dimmsShown && dimmsHidden > 0) f.leftOut += (f.leftOut.empty() ? L"" : L" · ") + std::wstring(L"RAM 외 ") + std::to_wstring(dimmsHidden) + L"개";
    return f;
}

// What a dock window keeps room for (SensorClient::PanelRoomHint, Dock::Metrics): the drives, the memory
// modules, the VRAM block and its temperature row.
struct PanelRoom {
    int drives = 0, dimms = 0;
    bool vram = false, vramTemp = false;
    bool operator==(const PanelRoom&) const = default;
};
// ...with the sensor settings applied on top of what was counted (that count never shrinks: what is hidden
// now keeps no room, and gets it back when shown again)
inline PanelRoom ShownRoom(PanelRoom r, const ShowItems& show, bool nvmlVramTempSeen) {
    if (!show.ssd) r.drives = 0;
    if (!show.DimmsShown()) r.dimms = 0;
    r.vramTemp = r.vram && show.VramTempShown(nvmlVramTempSeen);
    r.vram = r.vram && show.vram;
    return r;
}
// ...the third argument of ShownRoom for a window's room: the VRAM temperature row from NVML (no HWiNFO read)
// kept room for once NVML gave one at a panel sample, and from the start on an NVIDIA adapter - whether its NVML
// gives one is known only at the first panel sample, after the window was built (one built without the row
// would not show it before the next rebuild); that room then stays (a window's room never shrinks)
inline bool NvmlVramTempRoom(bool nvmlTempSeen, bool nvidiaAdapter) { return nvmlTempSeen || nvidiaAdapter; }
// ...in rows: `cols` a row, at most maxDimmRows module rows and maxDriveRows drive rows; at least one drive
// row while the drives are shown (one may come: a drive the first count missed); vram 0 / 1 / 2 as PanelVramDip
struct PanelRoomRows { int dimmRows = 0, driveRows = 0, vram = 0; };
inline PanelRoomRows RoomRows(const PanelRoom& r, bool drivesShown, int cols, int maxDimmRows, int maxDriveRows) {
    PanelRoomRows rr;
    const int dimmRows = (r.dimms + cols - 1) / cols, driveRows = (r.drives + cols - 1) / cols;
    rr.dimmRows = dimmRows < maxDimmRows ? dimmRows : maxDimmRows;
    rr.driveRows = !drivesShown ? 0 : driveRows < 1 ? 1 : driveRows > maxDriveRows ? maxDriveRows : driveRows;
    rr.vram = !r.vram ? 0 : r.vramTemp ? 2 : 1;
    return rr;
}

// What the panel shows in the room its window was built with (room: DIP; roomVram: the VRAM block that room
// was made with, 0 / 1 / 2): the VRAM block (wantVram: 0 none or hidden, 1 its line and bar, 2 and its
// temperature row) as far as the room was made for it, the drives next (as many rows as fit), then the
// module rows in what is left (maxDimmRows at most); what does not fit is counted ("외 N개"). A block, or a
// temperature row, the window was built without is shown only where it fits beside every row shown without
// it - it pushes nothing out.
struct PanelFit {
    int dimmRows = 0, driveRows = 0, vram = 0;
    int drivesShown = 0, drivesHidden = 0, dimmsShown = 0, dimmsHidden = 0;
    float height = 0;
};
inline PanelFit FitPanel(float room, int cols, int drives, int dimms, int maxDimmRows, int wantVram, int roomVram) {
    PanelFit f;
    f.vram = wantVram < roomVram ? wantVram : roomVram;
    const int wantDriveRows = (drives + cols - 1) / cols;
    while (f.driveRows < wantDriveRows && PanelHeightDip(0, f.driveRows + 1, f.vram) <= room) ++f.driveRows;
    const int dimmRows = (dimms + cols - 1) / cols;
    f.dimmRows = dimmRows < maxDimmRows ? dimmRows : maxDimmRows;
    while (f.dimmRows > 0 && PanelHeightDip(f.dimmRows, f.driveRows, f.vram) > room) --f.dimmRows;
    for (int k = wantVram; k > f.vram; --k)
        if (PanelHeightDip(f.dimmRows, f.driveRows, k) <= room) { f.vram = k; break; }
    f.drivesShown = drives < f.driveRows * cols ? drives : f.driveRows * cols;
    f.drivesHidden = drives - f.drivesShown;
    f.dimmsShown = dimms < f.dimmRows * cols ? dimms : f.dimmRows * cols;
    f.dimmsHidden = dimms - f.dimmsShown;
    f.height = PanelHeightDip(f.dimmRows, f.driveRows, f.vram);
    return f;
}

// The CPU's name for the panel's card, from the cell's short one (hw::CpuName drops "AMD ", "Intel(R) ",
// "Core(TM) ", "(R)", "(TM)", " Processor", "N-Core" and the clock): the brand put back. "Ryzen 7 9800X3D"
// -> "AMD Ryzen 7 9800X3D"; "i7-13700K", "Ultra 7 155H", "7 150U" (Core series 1) -> "Intel Core ...";
// "12th Gen i7-1260P" -> "Intel Core i7-1260P" (the generation is in the model number); "Xeon W-2295",
// "Pentium Gold 7505", "Celeron N4020" -> "Intel ..."; anything else as it is.
inline std::wstring PanelCpuName(std::wstring n) {
    auto starts = [&](const wchar_t* p) { return n.rfind(p, 0) == 0; };
    // "12th Gen ", "13th Gen ": the generation, which the model number carries too
    {
        size_t i = 0;
        while (i < n.size() && IsDigit(n[i])) ++i;
        if (i > 0 && n.compare(i, 7, L"th Gen ") == 0) n.erase(0, i + 7);
    }
    if (starts(L"Ryzen") || starts(L"Threadripper") || starts(L"Athlon") || starts(L"EPYC")) return L"AMD " + n;
    if (starts(L"Ultra ") || (n.size() > 3 && n[0] == L'i' && n[1] >= L'3' && n[1] <= L'9' && n[2] == L'-') ||
        (n.size() > 2 && IsDigit(n[0]) && n[1] == L' '))
        return L"Intel Core " + n;
    if (starts(L"Xeon") || starts(L"Pentium") || starts(L"Celeron") || starts(L"Atom")) return L"Intel " + n;
    return n;
}

// The elevated helper's reason for having no CPU temperature (LDSensorBlock::status), for the CPU card,
// where one line of about 110 - 180 DIP holds it: "CPU 온도: " goes (the card is the CPU's, the line its
// temperature's), and the helper's one reason today (sensors_main.cpp: "CPU 온도: 지원하지 않는 CPU이거나
// PawnIO가 없습니다", about 250 DIP) is said short so that its cause is not the part cut off. Others as they are.
inline std::wstring CpuCardStatus(std::wstring s) {
    const std::wstring prefix = L"CPU 온도: ";
    if (s.rfind(prefix, 0) == 0) s.erase(0, prefix.size());
    if (s == L"지원하지 않는 CPU이거나 PawnIO가 없습니다") return L"PawnIO 없음·미지원 CPU";
    return s;
}

// ---- timing ------------------------------------------------------------------------------------------------

// A reading taken every `period` ms by the clock, from a thread that wakes every second or two: due at
// the first wake on or after its time (100 ms early counts: tick-count granularity), keeping the cadence
// (5 s at 2 s wakes: 0, 6, 10, 16, 20 s - 5 s on average); after a long gap, from then on.
struct Period {
    uint64_t next = 0;
    bool started = false;
    bool Due(uint64_t nowMs, uint64_t periodMs) {
        if (started && nowMs + 100 < next) return false;
        next = (started && next + periodMs > nowMs) ? next + periodMs : nowMs + periodMs;
        started = true;
        return true;
    }
    // taken now by other means (the panel's drive list read the system drive): the next one a period on
    void Mark(uint64_t nowMs, uint64_t periodMs) {
        next = nowMs + periodMs;
        started = true;
    }
};

// Whether the panel counts as open for the sensor thread: at once when a dock wants it, and still for
// `linger` seconds after the last one stopped (a pointer passing over the cell does not flip the sampling
// back and forth). Update says when it just opened (the sensor thread is woken for a full sample then).
struct PanelLinger {
    bool open = false;
    double closedSince = -1;
    bool Update(bool wanted, double now, double linger = 5.0) {
        if (wanted) {
            closedSince = -1;
            if (!open) { open = true; return true; }
            return false;
        }
        if (open) {
            if (closedSince < 0) closedSince = now;
            if (now - closedSince >= linger) { open = false; closedSince = -1; }
        }
        return false;
    }
};

}   // namespace sensorlogic
