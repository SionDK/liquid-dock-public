#include "hwinfo.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <map>
#ifdef _WIN32
#include <tlhelp32.h>
#include <winternl.h>
#include <memory>
#include <new>
#endif

namespace hwinfo {

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

// The leading number of a value: "52.4", "52,4" (decimal comma), "52.4 °C". False when there is none.
bool LeadingNumber(const std::wstring& s, double& out) {
    std::string t;
    for (wchar_t c : s) {
        if ((c >= L'0' && c <= L'9') || ((c == L'-' || c == L'+') && t.empty())) t += (char)c;
        else if (c == L'.' || c == L',') t += '.';
        else if (c == L' ' || c == 0x00A0 || c == 0x202F) { if (!t.empty()) break; }   // spaces before the unit
        else break;
    }
    if (t.empty() || t == "-" || t == "+") return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && end != t.c_str();
}

}   // namespace

bool ParseTemperature(const std::wstring& value, const std::wstring& raw, float& celsius) {
    // HWiNFO writes temperatures as "52.4 °C", or "126.3 °F" when it is set to Fahrenheit
    wchar_t unit = 0;
    for (size_t i = 0; i < value.size() && !unit; ++i) {
        if (value[i] == 0x2103) unit = L'C';          // ℃
        else if (value[i] == 0x2109) unit = L'F';     // ℉
        else if (value[i] == 0x00B0) {                // °, possibly followed by a space
            size_t j = i + 1;
            while (j < value.size() && value[j] == L' ') ++j;
            if (j < value.size()) {
                wchar_t u = (wchar_t)std::towupper(value[j]);
                if (u == L'C' || u == L'F') unit = u;
            }
            if (!unit) return false;
        }
    }
    if (!unit) return false;
    double v = 0;
    if (!LeadingNumber(raw, v) && !LeadingNumber(value, v)) return false;
    if (unit == L'F') v = (v - 32.0) * 5.0 / 9.0;
    if (!(v > -40.0 && v < 150.0)) return false;
    celsius = (float)v;
    return true;
}

int RankCpu(const std::wstring& sensor, const std::wstring& label, const std::wstring& prefer) {
    const std::wstring l = Lower(label), s = Lower(sensor);
    if (!prefer.empty() && l == Lower(prefer)) return 0;
    if (l == L"cpu (tctl/tdie)") return 1;                    // AMD Ryzen
    if (l == L"cpu package") return 2;                        // Intel
    if (l == L"cpu die (average)") return 3;
    if (l == L"cpu (tdie)" || l == L"cpu (tctl)") return 4;
    const bool cpuSensor = s.rfind(L"cpu [", 0) == 0;          // HWiNFO's own CPU sensors: "CPU [#0]: ..."
    // the integrated graphics, reported in the CPU sensor (Intel's "CPU GT Cores (Graphics)", AMD's
    // "GFX"): the GPU's temperature, never the CPU's (as for HWiNFO's "GPU [#0]" sensors)
    if (cpuSensor && (l.find(L"graphics") != std::wstring::npos || l.find(L"gpu") != std::wstring::npos ||
                      l.find(L"gfx") != std::wstring::npos || l.find(L"gt cores") != std::wstring::npos ||
                      l.find(L"그래픽") != std::wstring::npos))
        return -1;
    if (cpuSensor && l == L"core max") return 5;
    if (cpuSensor && l.find(L"cpu") != std::wstring::npos) return 6;
    if (l == L"cpu") return 7;                                // the motherboard's (Super I/O) CPU sensor
    // any other temperature of HWiNFO's own CPU sensors: a HWiNFO in another language names its
    // readings in that language (Korean "코어 온도" for Core Temperatures), and one ticked there is
    // the user's choice. Not the distance to TjMAX: a margin, not a temperature.
    if (cpuSensor && l.find(L"tjmax") == std::wstring::npos) return 8;
    return -1;
}

int PickCpu(const std::vector<Entry>& entries, const std::wstring& prefer, float& celsius) {
    int best = -1, bestRank = INT_MAX;
    float bestC = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        int rank = RankCpu(e.sensor, e.label, prefer);
        if (rank < 0 || rank >= bestRank) continue;           // equal rank: the earlier reading stays
        float c = 0;
        if (!ParseTemperature(e.value, e.raw, c)) continue;
        best = (int)i; bestRank = rank; bestC = c;
    }
    if (best >= 0) celsius = bestC;
    return best;
}

namespace {

// Digits at s[i]: their value (4 digits at most), i past them; false when there are none.
bool Number(const std::wstring& s, size_t& i, int& n) {
    const size_t b = i;
    n = 0;
    while (i < s.size() && s[i] >= L'0' && s[i] <= L'9' && i - b < 4) n = n * 10 + (s[i++] - L'0');
    return i > b;
}

std::wstring TrimSpaces(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == L' ') ++a;
    while (b > a && s[b - 1] == L' ') --b;
    return s.substr(a, b - a);
}

}   // namespace

int RankDimm(const std::wstring& sensor, const std::wstring& label, int& slot, std::wstring& locator) {
    const std::wstring s = Lower(sensor), l = Lower(label);
    locator.clear();
    {   // DDR5: "DDR5 DIMM [#1] (P0 CHANNEL A/DIMM 1)", "LPDDR5 DIMM [#0] (...)"
        size_t i = s.rfind(L"lp", 0) == 0 ? 2 : 0;
        int gen = 0, n = 0;
        if (s.compare(i, 3, L"ddr") == 0 && (i += 3, Number(s, i, gen)) && s.compare(i, 8, L" dimm [#") == 0 &&
            (i += 8, Number(s, i, n)) && i < s.size() && s[i] == L']') {
            slot = n;
            const size_t open = sensor.rfind(L'('), close = sensor.rfind(L')');
            if (open != std::wstring::npos && close != std::wstring::npos && close > open && open > i)
                locator = TrimSpaces(sensor.substr(open + 1, close - open - 1));
            if (l.find(L"pmic") != std::wstring::npos) return -1;   // the power chip's own temperature
            if (l == L"spd hub temperature") return 0;
            if (l.find(L"spd") != std::wstring::npos || l.find(L"hub") != std::wstring::npos) return 1;
            return 2;
        }
    }
    {   // DDR4: sensor "DIMM Temperature Sensor", label "DIMM[0] Temperature" (or "Dimm [0] Temperature")
        const size_t d = l.find(L"dimm");
        if (d != std::wstring::npos && s.find(L"dimm") != std::wstring::npos) {
            size_t i = d + 4;
            while (i < l.size() && l[i] == L' ') ++i;
            int n = 0;
            if (i < l.size() && l[i] == L'[' && (++i, Number(l, i, n)) && i < l.size() && l[i] == L']') {
                slot = n;
                return 3;
            }
        }
    }
    return -1;
}

std::vector<DimmPick> PickDimms(const std::vector<Entry>& entries) {
    struct Best { DimmPick pick; int rank; };
    std::vector<Best> best;   // one per slot
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        int slot = 0;
        std::wstring locator;
        const int rank = RankDimm(e.sensor, e.label, slot, locator);
        if (rank < 0) continue;
        float c = 0;
        if (!ParseTemperature(e.value, e.raw, c)) continue;
        Best* same = nullptr;
        for (auto& b : best) if (b.pick.slot == slot) same = &b;
        if (same && same->rank <= rank) continue;            // equal rank: the earlier reading stays
        DimmPick p;
        p.entry = (int)i; p.slot = slot; p.celsius = c; p.locator = locator;
        if (same) *same = { p, rank };
        else best.push_back({ p, rank });
    }
    std::stable_sort(best.begin(), best.end(), [](const Best& a, const Best& b) { return a.pick.slot < b.pick.slot; });
    std::vector<DimmPick> out;
    for (auto& b : best) if (out.size() < 16) out.push_back(b.pick);
    return out;
}

namespace {

// A shown amount without its digit groups: a space between digits that exactly three digits follow ("3 412"),
// and for megabytes a comma there ("3,412 MB"; a figure in GB or % has a decimal sign: "1,9 GB").
std::wstring Ungrouped(const std::wstring& s, bool comma) {
    auto digit = [&](size_t k) { return k < s.size() && s[k] >= L'0' && s[k] <= L'9'; };
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        const bool sep = (c == L',' && comma) || c == L' ' || c == 0x00A0 || c == 0x202F;
        if (sep && i > 0 && digit(i - 1) && digit(i + 1) && digit(i + 2) && digit(i + 3) && !digit(i + 4)) continue;
        o += c;
    }
    return o;
}

}   // namespace

bool ParseAmount(const std::wstring& value, const std::wstring& raw, float& amount, wchar_t& unit) {
    // the unit: what the shown value ends with ("3,412 MB", "1.9 GB", "41.5 %")
    size_t end = value.size();
    while (end > 0 && (value[end - 1] == L' ' || value[end - 1] == 0x00A0 || value[end - 1] == 0x202F)) --end;
    wchar_t u = 0;
    if (end > 0 && value[end - 1] == L'%') u = L'%';
    else if (end > 1 && std::towupper(value[end - 1]) == L'B') {
        const wchar_t k = (wchar_t)std::towupper(value[end - 2]);
        if (k == L'M' || k == L'G') u = k;
    }
    if (!u) return false;
    // The shown number, and the bare one where it is that number before rounding. A bare number that is not
    // (another unit, another notation - HWiNFO's amounts were not seen in a key) is left alone: the unit read
    // above belongs to the shown one.
    double shown = 0, bare = 0;
    const bool haveShown = LeadingNumber(Ungrouped(value.substr(0, end), u == L'M'), shown);
    const bool haveBare = LeadingNumber(raw, bare);
    if (!haveShown && !haveBare) return false;
    double v = haveBare && (!haveShown || std::fabs(bare - shown) <= 0.51) ? bare : shown;
    if (u == L'G') v *= 1024.0;
    if (!(v >= 0.0) || (u == L'%' ? v > 100.5 : v > 4.0e6)) return false;   // more than 100 %, or than 4 TB
    amount = (float)v;
    unit = u == L'%' ? L'%' : L'M';
    return true;
}

namespace {

// HWiNFO's GPU sensors: "GPU [#0]: ...", or with one letter in front ("dGPU [#0]", "iGPU [#1]"). s: lower case.
bool GpuSensor(const std::wstring& s, int& gpu) {
    size_t i = 0;
    if (s.rfind(L"gpu", 0) != 0) {
        if (s.empty() || s[0] < L'a' || s[0] > L'z') return false;
        i = 1;
    }
    if (s.compare(i, 6, L"gpu [#") != 0) return false;
    i += 6;
    return Number(s, i, gpu) && i < s.size() && s[i] == L']';
}

}   // namespace

int RankGpuMemory(const std::wstring& sensor, const std::wstring& label, wchar_t kind, int& gpu) {
    const std::wstring s = Lower(sensor), l = Lower(label);
    if (!GpuSensor(s, gpu)) return -1;
    auto has = [&](const wchar_t* w) { return l.find(w) != std::wstring::npos; };
    if (!has(L"memory") && !has(L"메모리") && !has(L"vram")) return -1;
    if (kind == L'T') {
        if (has(L"limit") || has(L"tjmax") || has(L"한계") || has(L"제한")) return -1;   // a threshold or a margin
        if (has(L"vrm") || has(L"vr ") || has(L"power") || has(L"전원")) return -1;      // the memory's power stage, not the memory
        if (l == L"gpu memory junction temperature") return 0;
        if (has(L"memory junction") || (has(L"메모리") && has(L"접합"))) return 1;
        if (l == L"gpu memory temperature") return 2;
        return 3;
    }
    // amounts: not the shared ("dynamic") memory, not what is free, not the memory controller's load
    for (const wchar_t* w : { L"dynamic", L"동적", L"shared", L"공유", L"available", L"free", L"가능", L"여유", L"controller", L"컨트롤러" })
        if (has(w)) return -1;
    const bool usage = has(L"usage") || has(L"used") || has(L"사용");
    if (kind == L'M') {
        if (l == L"gpu memory allocated") return 0;
        if (l == L"gpu d3d memory dedicated") return 1;
        if (l == L"gpu memory usage") return 2;
        if (has(L"allocated") || has(L"할당")) return 3;
        if (has(L"dedicated") || has(L"전용")) return 4;
        return usage ? 5 : -1;
    }
    if (kind == L'%') return l == L"gpu memory usage" ? 0 : (usage ? 1 : -1);
    return -1;
}

std::vector<GpuMemPick> PickGpuMemory(const std::vector<Entry>& entries, std::vector<int>* used) {
    // lead: the sensor name's first letter ('d', 'i'; 'g' without one) - "iGPU [#0]" and "dGPU [#0]" are two GPUs
    struct Best { GpuMemPick pick; wchar_t lead = 0; int temp = INT_MAX, amount = INT_MAX, share = INT_MAX; };   // the ranks held
    std::vector<Best> best;   // one per GPU
    if (used) used->clear();
    int taken = 0;
    for (size_t i = 0; i < entries.size() && taken < kMaxGpuMemoryEntries; ++i) {
        const Entry& e = entries[i];
        float v = 0;
        wchar_t kind = L'T';
        if (!ParseTemperature(e.value, e.raw, v) && !ParseAmount(e.value, e.raw, v, kind)) continue;
        int gpu = 0;
        const int rank = RankGpuMemory(e.sensor, e.label, kind, gpu);
        if (rank < 0) continue;
        ++taken;
        if (used) used->push_back((int)i);
        const wchar_t lead = (wchar_t)std::towlower(e.sensor[0]);   // RankGpuMemory took the name: not empty
        Best* b = nullptr;
        for (auto& x : best) if (x.pick.gpu == gpu && x.lead == lead) b = &x;
        if (!b) { best.push_back({}); b = &best.back(); b->lead = lead; b->pick.gpu = gpu; b->pick.sensor = e.sensor; }
        GpuMemPick& p = b->pick;
        if (kind == L'T') {
            // 0 °C is no reading: HWiNFO shows it for a discrete GPU that is asleep (a hybrid laptop's)
            if (v > 0 && (rank < b->temp || (rank == b->temp && v > p.celsius))) { b->temp = rank; p.hasTemp = true; p.celsius = v; }
        } else if (kind == L'M') {
            if (rank < b->amount) { b->amount = rank; p.hasUsed = true; p.usedMB = v; }
        } else if (rank < b->share) { b->share = rank; p.hasUsage = true; p.usagePct = v; }
    }
    // by number; of two with one number the integrated GPU last (the block is for a GPU with memory of its own)
    std::stable_sort(best.begin(), best.end(), [](const Best& a, const Best& b) {
        return a.pick.gpu != b.pick.gpu ? a.pick.gpu < b.pick.gpu : (a.lead != L'i' && b.lead == L'i');
    });
    std::vector<GpuMemPick> out;
    for (auto& b : best) if (out.size() < 4) out.push_back(b.pick);
    return out;
}

#ifdef _WIN32

namespace {

constexpr wchar_t kKey[] = L"Software\\HWiNFO64\\VSB";

bool ReadValue(HKEY k, const std::wstring& name, std::wstring& out) {
    wchar_t buf[512];
    DWORD cb = sizeof(buf);
    if (RegGetValueW(k, nullptr, name.c_str(), RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return false;
    out = buf;
    return true;
}

bool ReadEntry(HKEY k, int n, Entry& e) {
    std::wstring i = std::to_wstring(n);
    e.index = n;
    if (!ReadValue(k, L"Label" + i, e.label) || !ReadValue(k, L"Value" + i, e.value)) return false;
    ReadValue(k, L"Sensor" + i, e.sensor);
    ReadValue(k, L"ValueRaw" + i, e.raw);
    return true;
}

// Every reported reading, in HWiNFO's order. Indices can have gaps: HWiNFO drops the values of a
// sensor that stops reporting.
std::vector<Entry> ReadAll(HKEY k) {
    std::map<int, Entry> byIndex;
    wchar_t name[64];
    BYTE data[1024];
    for (DWORD i = 0;; ++i) {
        DWORD cchName = 64, cbData = sizeof(data) - sizeof(wchar_t), type = 0;
        LONG rc = RegEnumValueW(k, i, name, &cchName, nullptr, &type, data, &cbData);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc == ERROR_MORE_DATA) continue;                   // too long: not one of ours
        // any other error ends the scan: a key deleted while it is read (HWiNFO rebuilding its Gadget
        // key, a settings reset) answers ERROR_KEY_DELETED for every index, never ERROR_NO_MORE_ITEMS
        if (rc != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;                          // not text: not one of ours
        // "ValueRaw12" -> field "ValueRaw", index 12
        size_t len = cchName, d = len;
        while (d > 0 && name[d - 1] >= L'0' && name[d - 1] <= L'9') --d;
        if (d == len || d == 0 || len - d > 6) continue;
        std::wstring field(name, d);
        int n = _wtoi(name + d);
        size_t chars = cbData / sizeof(wchar_t);
        while (chars > 0 && ((const wchar_t*)data)[chars - 1] == 0) --chars;
        std::wstring text((const wchar_t*)data, chars);
        Entry& e = byIndex[n];
        e.index = n;
        if (field == L"Sensor") e.sensor = text;
        else if (field == L"Label") e.label = text;
        else if (field == L"Value") e.value = text;
        else if (field == L"ValueRaw") e.raw = text;
    }
    std::vector<Entry> out;
    out.reserve(byIndex.size());
    for (auto& [n, e] : byIndex) if (!e.label.empty()) out.push_back(std::move(e));
    return out;
}

ULONGLONG U64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

bool IsHWiNFO(std::wstring exe) {
    exe = Lower(std::move(exe));   // HWiNFO64.exe, HWiNFO32.exe, ...
    return exe.rfind(L"hwinfo", 0) == 0 && exe.size() > 4 && exe.compare(exe.size() - 4, 4, L".exe") == 0;
}

// This process's Terminal Services session: only an HWiNFO of the same session counts. The Gadget key is
// the user's own (HKCU); with fast user switching another user's HWiNFO, running in their session, says
// nothing about whether this user's key is still written (it is not: theirs closed, its values left).
DWORD OwnSession() {
    static const DWORD session = [] { DWORD s = 0; return ProcessIdToSessionId(GetCurrentProcessId(), &s) ? s : 0xFFFFFFFFu; }();
    return session;
}

// The process list a Toolhelp snapshot is made from (NtQuerySystemInformation), read straight: the same
// names at under half the cost (290 processes: 4.5 ms -> 1.9 ms). Read until HWiNFO's process
// is found and held (Reader::HWiNFOProcessRunning), and every 5 s while it does not run. 1: HWiNFO is among
// them in this session (pid: its process id), 0: it is not, -1: no list to be had. bytes: what the list
// took last time, so that one call does.
int ListedHWiNFO(ULONG& bytes, DWORD& pid) {
    using Query = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    constexpr NTSTATUS kTooSmall = (NTSTATUS)0xC0000004L;   // STATUS_INFO_LENGTH_MISMATCH
    if (!query) return -1;
    for (int tries = 0; tries < 3; ++tries) {   // the list can grow between two calls, a process starting
        if (bytes < 64 * 1024) bytes = 64 * 1024;
        std::unique_ptr<BYTE[]> buf(new (std::nothrow) BYTE[bytes]);
        if (!buf) return -1;
        ULONG need = 0;
        const NTSTATUS st = query(SystemProcessInformation, buf.get(), bytes, &need);
        if (st == kTooSmall) { const ULONG want = need > bytes ? need : bytes * 2; bytes = want + want / 8; continue; }
        if (st < 0) return -1;
        if (need) bytes = need + need / 8;   // follows the list as it grows or shrinks
        for (const BYTE* p = buf.get();;) {
            const auto* e = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(p);
            if (e->ImageName.Buffer && (OwnSession() == 0xFFFFFFFFu || e->SessionId == OwnSession()) &&
                IsHWiNFO(std::wstring(e->ImageName.Buffer, e->ImageName.Length / sizeof(wchar_t)))) {
                pid = (DWORD)(ULONG_PTR)e->UniqueProcessId;
                return 1;
            }
            if (!e->NextEntryOffset) return 0;
            p += e->NextEntryOffset;
        }
    }
    return -1;
}

bool SnapshotHasHWiNFO(DWORD& pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (!IsHWiNFO(pe.szExeFile)) continue;
        DWORD session = 0;   // as the list: this session's only (a process gone meanwhile: not counted)
        if (OwnSession() != 0xFFFFFFFFu && (!ProcessIdToSessionId(pe.th32ProcessID, &session) || session != OwnSession())) continue;
        found = true; pid = pe.th32ProcessID; break;
    }
    CloseHandle(snap);
    return found;
}

}   // namespace

Reader::~Reader() {
    if (process_) CloseHandle(process_);
}

// Whether HWiNFO runs, asked while the Gadget key is quiet - with HWiNFO writing a value only when it
// changes, that is much of the time while temperatures are steady. Once found in the process list, its
// process is held (SYNCHRONIZE) and asked with a zero wait (well under a microsecond) instead of reading
// the list every 5 s (0.8 - 3.2 ms on a test PC, 255 processes). When it exits the list is read at
// once (a restarted HWiNFO is found again); while it cannot be opened (an HWiNFO run as administrator, say)
// or does not run, every 5 s, as before. Only an HWiNFO of this session counts (OwnSession).
bool Reader::HWiNFOProcessRunning() {
    if (process_) {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return processRunning_ = true;
        CloseHandle(process_);
        process_ = nullptr;
        processCheckedAt_ = 0;
    }
    const ULONGLONG now = GetTickCount64();
    if (processCheckedAt_ && now - processCheckedAt_ < 5000) return processRunning_;
    processCheckedAt_ = now;
    DWORD pid = 0;
    const int listed = ListedHWiNFO(processListBytes_, pid);
    processRunning_ = listed >= 0 ? listed > 0 : SnapshotHasHWiNFO(pid);   // no list: the snapshot, as before
    if (processRunning_ && pid) process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process_) {
        // the process the list named, not another that took its id between the list and the open (HWiNFO
        // exited just then): its image is HWiNFO's, or it is not held (the list every 5 s; again at the next look)
        wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
        const wchar_t* exe = nullptr;
        if (QueryFullProcessImageNameW(process_, 0, path, &n)) { exe = wcsrchr(path, L'\\'); exe = exe ? exe + 1 : path; }
        if (!exe || !IsHWiNFO(exe)) { CloseHandle(process_); process_ = nullptr; }
    }
    return processRunning_;
}

CpuReading Reader::ReadCpu() {
    const Reading r = Read(false);
    CpuReading c;
    c.running = r.running;
    c.found = r.cpuFound;
    c.celsius = r.cpuCelsius;
    return c;
}

Reading Reader::Read(bool wantDimms) {
    Reading r;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) != ERROR_SUCCESS &&
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, kKey, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        cachedIndex_ = -1;
        dimmCache_.clear();
        gpuCache_.clear();
        nextScan_ = 0;
        return r;   // HWiNFO never reported to the Gadget on this PC
    }
    DWORD count = 0;
    FILETIME written{};
    if (RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, nullptr, nullptr, nullptr, &written) != ERROR_SUCCESS)
        count = 0;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const ULONGLONG now = GetTickCount64();
    // How long the key has been quiet. A clock set back since the last write (a time sync correcting a fast
    // clock, a manual change) puts that write ahead of it: how long ago is unknown, so 30 s - the process
    // check decides. And at least as long as this reader has seen the key unchanged, by the tick count,
    // which no clock change moves: values a closed HWiNFO left behind go stale whatever the clock did.
    const ULONGLONG nowT = U64(nowFt), wroteT = U64(written);
    if (wroteT != stamp_ || !stampSeenAt_) { stamp_ = wroteT; stampSeenAt_ = now; }
    double age = nowT >= wroteT ? (nowT - wroteT) / 1e7 : (wroteT - nowT > 20000000ull ? 30.0 : 0.0);
    const double unchanged = (now - stampSeenAt_) / 1000.0;
    if (unchanged > age) age = unchanged;
    Freshness f = Judge(count, age, [this] { return HWiNFOProcessRunning(); });
    r.running = f.running;
    if (!f.usable) {
        cachedIndex_ = -1;
        dimmCache_.clear();
        gpuCache_.clear();
        nextScan_ = 0;   // look again as soon as HWiNFO is back
        RegCloseKey(k);
        return r;
    }
    const bool scanDue = now >= nextScan_;
    bool cpuOk = false, dimmsOk = true, gpusOk = true;
    // fast path: the readings picked at the last scan, by index, if they are still the same ones
    if (!scanDue) {
        if (cachedIndex_ >= 0) {
            Entry e;
            if (ReadEntry(k, cachedIndex_, e) && e.label == cachedLabel_ && e.sensor == cachedSensor_)
                cpuOk = ParseTemperature(e.value, e.raw, r.cpuCelsius);
        }
        if (wantDimms) {
            for (const Cached& c : dimmCache_) {
                Entry e;
                DimmPick p;
                if (!ReadEntry(k, c.index, e) || e.label != c.label || e.sensor != c.sensor || !ParseTemperature(e.value, e.raw, p.celsius)) {
                    dimmsOk = false;
                    break;
                }
                p.entry = c.index; p.slot = c.slot; p.locator = c.locator;
                r.dimms.push_back(p);
            }
            // the GPUs' memory readings found at the last scan, picked among again (the same rules)
            std::vector<Entry> found;
            for (const Cached& c : gpuCache_) {
                Entry e;
                if (!ReadEntry(k, c.index, e) || e.label != c.label || e.sensor != c.sensor) { gpusOk = false; break; }
                found.push_back(std::move(e));
            }
            if (gpusOk && !found.empty()) r.gpus = PickGpuMemory(found);
        }
    }
    // full scan, for the CPU, the memory and the GPU memory pickers alike: right away when a picked reading
    // went away, every 10 s otherwise (a better reading ticked later is picked up), and every 3 s while no
    // CPU temperature is reported - not for missing memory or GPU memory readings (most people never tick
    // them: a scan every 3 s for good)
    if (scanDue || (cachedIndex_ >= 0 && !cpuOk) || (wantDimms && (!dimmsOk || !gpusOk))) {
        std::vector<Entry> all = ReadAll(k);
        const int i = PickCpu(all, prefer_, r.cpuCelsius);
        cachedIndex_ = i >= 0 ? all[i].index : -1;
        cpuOk = i >= 0;
        if (i >= 0) { cachedSensor_ = all[i].sensor; cachedLabel_ = all[i].label; }
        dimmCache_.clear();
        r.dimms.clear();
        for (DimmPick p : PickDimms(all)) {
            const Entry& e = all[p.entry];
            Cached c;
            c.index = e.index; c.sensor = e.sensor; c.label = e.label; c.slot = p.slot; c.locator = p.locator;
            dimmCache_.push_back(c);
            p.entry = e.index;
            if (wantDimms) r.dimms.push_back(p);
        }
        gpuCache_.clear();
        r.gpus.clear();
        std::vector<int> used;
        std::vector<GpuMemPick> gpus = PickGpuMemory(all, &used);
        for (int at : used) {
            Cached c;
            c.index = all[at].index; c.sensor = all[at].sensor; c.label = all[at].label;
            gpuCache_.push_back(c);
        }
        if (wantDimms) r.gpus = std::move(gpus);
        nextScan_ = now + (i >= 0 ? 10000 : 3000);
    }
    r.cpuFound = cpuOk;
    RegCloseKey(k);
    return r;
}

#endif   // _WIN32

}   // namespace hwinfo
