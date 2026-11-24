#pragma once
// CPU and memory temperatures and the GPU's memory (VRAM) readings from HWiNFO, through its documented
// "HWiNFO Gadget" interface: for every reading the user ticks "Report value in Gadget" (Sensors > Configure Sensors > HWiNFO Gadget), HWiNFO
// writes SensorN / LabelN / ValueN / ValueRawN under HKCU\Software\HWiNFO64\VSB. HWiNFO 8.54 rewrites a
// value only when it changes (measured on a test PC, 2026-11-13: the key's write time stood still
// for 20 s and more while the CPU temperature was steady), so a quiet key is no sign of a stopped HWiNFO.
// The dock needs no driver and no admin rights for it, and it shows the same value RTSS or a Rainmeter
// skin gets from HWiNFO. (HWiNFO's shared memory is not used: its terms keep that interface out of
// open-source code, and the free HWiNFO turns it off after 12 h.)
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace hwinfo {

// One reading HWiNFO reports to the Gadget key.
struct Entry {
    int index = -1;            // the N in LabelN
    std::wstring sensor;       // "CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced"
    std::wstring label;        // "CPU (Tctl/Tdie)"
    std::wstring value;        // formatted with its unit, "52.4 °C"
    std::wstring raw;          // the bare number, "52.4"
};

// Temperature in °C from a reported value; false when it is not a temperature (or implausible).
bool ParseTemperature(const std::wstring& value, const std::wstring& raw, float& celsius);
// How well a reading stands for "the CPU temperature": 0 best, -1 not a CPU reading. Known English
// labels first; then any reading of HWiNFO's own CPU sensors ("CPU [#0]: ..."), whatever the language.
// prefer: a label the user picked (settings.ini hwinfoCpuLabel), matched first.
int RankCpu(const std::wstring& sensor, const std::wstring& label, const std::wstring& prefer);
// The best CPU temperature among the entries: its position in `entries`, or -1.
int PickCpu(const std::vector<Entry>& entries, const std::wstring& prefer, float& celsius);

// How well a reading stands for a memory module's temperature: 0 best, -1 not one. HWiNFO keeps sensor
// names in English whatever its language (only labels are translated):
//   DDR5: a sensor per module, "DDR5 DIMM [#n] (<Bank Locator>/<Device Locator>)" (LPDDR5 too):
//         0 label "SPD Hub Temperature", 1 a label with "SPD" or "Hub" (translated), 2 its other
//         temperatures but the PMIC's;
//   DDR4: sensor "DIMM Temperature Sensor", labels "DIMM[n] Temperature": 3.
// A motherboard's "DIMM"/"DDR" probe (Super I/O) is none of these. slot: HWiNFO's module number;
// locator: the "(Bank/Device)" of a DDR5 sensor name, trimmed (empty for DDR4).
int RankDimm(const std::wstring& sensor, const std::wstring& label, int& slot, std::wstring& locator);

struct DimmPick {
    int entry = -1;            // position in `entries` (PickDimms), the Gadget index N (Reader::Read)
    int slot = 0;              // HWiNFO's module number
    float celsius = 0;
    std::wstring locator;      // "P0 CHANNEL A/DIMM 1", empty when the sensor name has none
};
// One temperature per module (the best rank, then the earlier reading), temperatures only, ordered by
// HWiNFO's module number, 16 at most.
std::vector<DimmPick> PickDimms(const std::vector<Entry>& entries);

// An amount or a share from a reported value: "3,412 MB" / "1.9 GB" -> MB, unit L'M'; "41.5 %" -> unit
// L'%'. The bare number (raw) when it is the shown one before rounding, otherwise the shown one (digit
// groups dropped: "3,412", "3 412"). False for anything else (a temperature, a voltage...).
bool ParseAmount(const std::wstring& value, const std::wstring& raw, float& amount, wchar_t& unit);
// How well a reading of one of HWiNFO's GPU sensors stands for that GPU's own memory (VRAM): 0 best, -1
// not one. Sensor names stay English: "GPU [#n]: <model>: <board>", with a letter in front since HWiNFO
// tells discrete from integrated GPUs ("dGPU [#0]: NVIDIA GeForce <model>: <board>" in a
// Korean HWiNFO); labels are translated ("GPU 메모리 접합 온도" there).
// kind: what the value is - L'T' a temperature, L'M' an amount (MB), L'%' a share.
//   L'T'  0 "GPU Memory Junction Temperature", 1 a label with "memory junction" (Korean: 메모리 + 접합),
//         2 "GPU Memory Temperature", 3 any other temperature with "memory" / 메모리 / "VRAM" in its label
//         (per-chip readings) - never the GPU's own or its hot spot's (no "memory" in those), nor a limit
//         or the memory's power stage (VRM);
//   L'M'  0 "GPU Memory Allocated", 1 "GPU D3D Memory Dedicated", 2 "GPU Memory Usage" (AMD: in MB), then
//         translated labels by their words (allocated 3, dedicated 4, usage 5: guesses, not seen in a key);
//   L'%'  0 "GPU Memory Usage", 1 a memory label with usage / used / 사용.
// Never the shared ("dynamic") memory, what is free or available, or the memory controller's load.
// gpu: HWiNFO's GPU number.
int RankGpuMemory(const std::wstring& sensor, const std::wstring& label, wchar_t kind, int& gpu);

struct GpuMemPick {
    int gpu = 0;               // HWiNFO's GPU number (an "iGPU [#0]" and a "dGPU [#0]" are two picks)
    std::wstring sensor;       // the sensor's name (it holds the GPU's model)
    bool hasTemp = false, hasUsed = false, hasUsage = false;
    float celsius = 0;         // the memory's (junction) temperature
    float usedMB = 0;          // dedicated memory in use
    float usagePct = 0;        // ...as a share
};
// What each GPU sensor reports about its memory: per GPU (its number and the letter in front) and kind the
// best rank (equal ranks: the hottest temperature - per-chip readings -, the earlier amount), ordered by GPU
// number (of two with one number the integrated one last), 4 GPUs at most. A temperature of 0 °C is none (a
// sleeping GPU). Only the first kMaxGpuMemoryEntries such readings are looked at. used (optional): their
// positions in `entries`.
constexpr int kMaxGpuMemoryEntries = 24;
std::vector<GpuMemPick> PickGpuMemory(const std::vector<Entry>& entries, std::vector<int>* used = nullptr);

// Power in W from a reported value: "71.234 W", "71,2 W" (decimal comma), "850 mW", "71W"; the bare number (raw)
// when it is the shown one before rounding. False for anything else (a temperature, a share in %, "Wh", MHz...)
// or a figure outside 0 .. 5000 W.
bool ParsePower(const std::wstring& value, const std::wstring& raw, float& watts);
// How well a reading stands for the CPU's package power (the "PC 상태" panel's CPU power): 0 best, -1 not one.
// HWiNFO's own CPU sensors only ("CPU [#0]: ..."; its sensor names stay English):
//   0 "CPU Package Power" (Intel, AMD), 1 "CPU PPT" (AMD: the socket's power the PPT limit tracks),
//   2 a translated label with package / 패키지 and power / 전력 / 전원 ("CPU 패키지 전력"),
//   3 any other label with "PPT".
// Never a limit or a current (limit / 한계 / 제한, TDC, EDC), a part of the package (IA / GT Cores, the graphics,
// the SoC, the cores - per core or together -, the memory, System Agent, Rest-of-Chip, uncore).
int RankCpuPower(const std::wstring& sensor, const std::wstring& label);
// The best CPU package power among the entries: its position in `entries`, or -1.
int PickCpuPower(const std::vector<Entry>& entries, float& watts);

struct CpuReading {
    bool running = false;      // HWiNFO is running and reporting to the Gadget key
    bool found = false;        // a CPU temperature is among the reported readings
    float celsius = 0;
};

struct Reading {
    bool running = false;      // HWiNFO is running and reporting to the Gadget key
    bool cpuFound = false;     // a CPU temperature is among the reported readings
    float cpuCelsius = 0;
    // the CPU's package power (asked for only: Read(..., true)): reported to the Gadget, and its watts
    bool cpuPowerFound = false;
    float cpuPowerW = 0;
    // the panel's readings (asked for only: Read(true))
    std::vector<DimmPick> dimms;   // memory temperatures; entry = Gadget index
    std::vector<GpuMemPick> gpus;  // the GPUs' own memory (VRAM): temperature, amount in use
};

// Values a closed HWiNFO left behind must not show up as a frozen temperature. A key written within 20 s
// counts as HWiNFO's own; a quieter one only while the HWiNFO process runs - for as long as it does,
// whatever the key's age: HWiNFO writes a value only when it changes, and switching its Gadget reporting
// off empties the key (count 0) rather than leaving values to go stale (seen 10-28).
struct Freshness { bool running = false; };
template <class IsProcessRunning>
Freshness Judge(unsigned valueCount, double ageSeconds, IsProcessRunning&& processRunning) {
    Freshness f;
    f.running = valueCount > 0 && (ageSeconds < 20.0 || processRunning());
    return f;
}

#ifdef _WIN32
// Polled from the dock's sensor thread, every one or two seconds.
class Reader {
public:
    explicit Reader(std::wstring preferLabel = {}) : prefer_(std::move(preferLabel)) {}
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    CpuReading ReadCpu();                // Read(false)
    // One open of the key: the picked CPU reading and (wantDimms: a panel is open) the picked memory
    // readings and the GPUs' memory readings, by their cached indices; a full scan, shared by the pickers,
    // every 10 s (3 s while no CPU temperature is found - not for missing memory, GPU or power readings) or as
    // soon as a cached reading changed. wantCpuPower (the panel's power draw shown): the picked CPU package
    // power too, by its cached index (one more reading, about 7 us); the first read that asks for it after one
    // that did not (or after a scan without it) scans at once. Not asked: not even picked at the scans.
    Reading Read(bool wantDimms, bool wantCpuPower = false);
private:
    struct Cached { int index = -1; std::wstring sensor, label; int slot = 0; std::wstring locator; };
    bool HWiNFOProcessRunning();
    // Read's parts, in its order: the key's age; the cached picks between scans; the full scan; forgetting
    void Forget();
    double KeyAge(FILETIME nowFt, FILETIME written, ULONGLONG now);
    void ReadCached(HKEY k, bool wantDimms, bool wantPower, Reading& r, bool& cpuOk, bool& dimmsOk, bool& gpusOk, bool& powerOk);
    void FullScan(HKEY k, bool wantDimms, bool wantPower, ULONGLONG now, Reading& r, bool& cpuOk);
    std::wstring prefer_;
    int cachedIndex_ = -1;               // the CPU reading picked at the last full scan
    std::wstring cachedSensor_, cachedLabel_;
    Cached powerCache_;                  // the CPU package power picked at it (index -1: none reported)
    bool powerPicked_ = false;           // ...and that scan was asked for it (false: the next read that asks scans)
    std::vector<Cached> dimmCache_;      // the memory readings picked at the last full scan
    std::vector<Cached> gpuCache_;       // the GPU memory readings found at it (index, sensor, label)
    ULONGLONG nextScan_ = 0;
    ULONGLONG stamp_ = 0, stampSeenAt_ = 0;   // the key's last write time, and the tick count it was first seen at
    ULONGLONG processCheckedAt_ = 0;     // the last look through the process list
    ULONG processListBytes_ = 0;         // what the process list took at the last look
    bool processRunning_ = false;
    HANDLE process_ = nullptr;           // HWiNFO's process, held once found: asked with a zero wait
};
#endif

}   // namespace hwinfo
