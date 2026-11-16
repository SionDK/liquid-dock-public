// The "PC 상태" panel's data rules (src/sensor_logic.h, no Windows headers): drive model names, memory slots
// from a raw SMBIOS table, HWiNFO's memory temperatures matched to slots, the panel's rows and footer counts,
// the GPU's memory block (VRAM: when it shows, where its amounts and temperature come from), the sensor
// thread's time-based periods, the panel-open linger, the dock cell's key and text, and the sensor settings
// (the source, the six show switches) with the panel's layout that follows from them. Any C++20 compiler:
//   g++ -std=c++20 tests/sensor_logic_test.cpp        cl /std:c++20 /EHsc /utf-8 tests\sensor_logic_test.cpp
#include "../src/sensor_logic.h"
#include "portable_check.h"
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

using namespace sensorlogic;

namespace {

// ---- a raw SMBIOS structure table, built the way firmware lays it out
struct Table {
    std::vector<uint8_t> bytes;
    void Add(uint8_t type, std::vector<uint8_t> formatted, const std::vector<std::string>& strings) {
        formatted[0] = type;
        formatted[1] = (uint8_t)formatted.size();
        bytes.insert(bytes.end(), formatted.begin(), formatted.end());
        if (strings.empty()) { bytes.push_back(0); bytes.push_back(0); return; }
        for (auto& s : strings) { bytes.insert(bytes.end(), s.begin(), s.end()); bytes.push_back(0); }
        bytes.push_back(0);
    }
    // a type 17 (SMBIOS 3.x, 0x28 bytes formatted): size in MB (0x7FFF + extended size from 32 GB - 1 MB on)
    void Memory(unsigned sizeMB, uint8_t formFactor, const std::string& device, const std::string& bank) {
        std::vector<uint8_t> f(0x28, 0);
        const unsigned size = sizeMB == 0xFFFF ? 0xFFFF : sizeMB >= 0x7FFF ? 0x7FFF : sizeMB;   // 0xFFFF: size unknown
        f[0x0C] = size & 0xFF; f[0x0D] = (size >> 8) & 0xFF;
        f[0x0E] = formFactor;
        f[0x10] = 1;   // Device Locator: string 1
        f[0x11] = 2;   // Bank Locator: string 2
        f[0x17] = 3;   // Manufacturer: string 3
        if (size == 0x7FFF) { f[0x1C] = sizeMB & 0xFF; f[0x1D] = (sizeMB >> 8) & 0xFF; f[0x1E] = (sizeMB >> 16) & 0xFF; }
        Add(17, f, { device, bank, "A-DATA Technology" });
    }
    void End() { Add(127, std::vector<uint8_t>(4, 0), {}); }
};

// a desktop board's table (an AMD AM5 board): 4 slots, modules in A2 and B2
Table DesktopBoard() {
    Table t;
    t.Add(0, std::vector<uint8_t>(0x1A, 0), { "American Megatrends International, LLC.", "F42" });   // BIOS information
    t.Memory(0, 0x02, "DIMM 0", "P0 CHANNEL A");
    t.Memory(32768, 0x09, "DIMM 1", "P0 CHANNEL A");
    t.Memory(0, 0x02, "DIMM 0", "P0 CHANNEL B");
    t.Memory(32768, 0x09, "DIMM 1", "P0 CHANNEL B");
    t.End();
    return t;
}

DimmTemp Temp(int slot, const wchar_t* locator, float c) {
    DimmTemp t;
    t.slot = slot; t.locator = locator; t.celsius = c;
    return t;
}

bool Same(float a, float b) { return std::fabs(a - b) < 0.001f; }

void Models() {
    Check(CleanDriveModel(L"", L"WD_BLACK SN850X 2000GB") == L"WD SN850X", "\"WD_BLACK SN850X 2000GB\" -> \"WD SN850X\"");
    Check(CleanDriveModel(L"", L"SAMSUNG MZVL21T0HCLR-00B00") == L"Samsung MZVL21T0HCLR-00B00",
          "\"SAMSUNG MZVL21T0HCLR-00B00\" -> \"Samsung MZVL21T0HCLR-00B00\" (the 1T0 inside the model code stays)");
    Check(CleanDriveModel(L"", L"Samsung SSD 870 EVO 1TB") == L"Samsung 870 EVO", "\"Samsung SSD 870 EVO 1TB\" -> \"Samsung 870 EVO\"");
    Check(CleanDriveModel(L"", L"Samsung SSD 990 PRO 2TB    ") == L"Samsung 990 PRO", "space padding goes");
    Check(CleanDriveModel(L"NVMe", L"Samsung SSD 980 1TB") == L"Samsung 980", "a vendor id that is the bus (\"NVMe\") goes");
    Check(CleanDriveModel(L"Samsung", L"SSD 980 PRO 1TB") == L"Samsung 980 PRO", "a real vendor id comes first");
    Check(CleanDriveModel(L"Samsung", L"Samsung SSD 980 PRO 1TB") == L"Samsung 980 PRO", "...not twice when the product id repeats it");
    Check(CleanDriveModel(L"", L"KINGSTON SA400S37480G") == L"Kingston SA400S37480G", "SATA KINGSTON: title case, the capacity inside the code stays");
    Check(CleanDriveModel(L"", L"KIOXIA-EXCERIA G2 SSD") == L"KIOXIA-EXCERIA G2", "a brand in capitals with its line (KIOXIA-EXCERIA) stays");
    Check(CleanDriveModel(L"", L"KIOXIA EXCERIA PLUS G4 2TB") == L"KIOXIA EXCERIA PLUS G4", "KIOXIA stays in capitals");
    Check(CleanDriveModel(L"", L"ADATA SX8200PNP") == L"ADATA SX8200PNP", "ADATA stays in capitals");
    Check(CleanDriveModel(L"", L"WDC WDS100T2B0A-00SM50") == L"WD WDS100T2B0A-00SM50", "\"WDC\" -> \"WD\"");
    Check(CleanDriveModel(L"", L"WD Blue SN580 1TB") == L"WD SN580", "WD's product lines go (WD Blue)");
    Check(CleanDriveModel(L"", L"Fanxiang S880 2TB") == L"Fanxiang S880", "Fanxiang S880 2TB -> Fanxiang S880");
    Check(CleanDriveModel(L"", L"Lexar SSD ARES 2TB") == L"Lexar ARES", "Lexar SSD ARES 2TB -> Lexar ARES");
    Check(CleanDriveModel(L"", L"Micron_2450_MTFDKBA512TFK") == L"Micron 2450 MTFDKBA512TFK", "underscores become spaces");
    Check(CleanDriveModel(L"", L"INTEL SSDPEKNW010T8") == L"Intel SSDPEKNW010T8", "only a standalone SSD word goes (SSDPEKNW stays)");
    Check(CleanDriveModel(L"", L"ST2000DM008-2FR102") == L"ST2000DM008-2FR102", "a hard disk's model code stays");
    Check(CleanDriveModel(L"", L"CT1000MX500SSD1 1.92TB 500G") == L"CT1000MX500SSD1", "several trailing capacities go (1.92TB, 500G)");
    Check(CleanDriveModel(L"", L"SSD 1TB") == L"SSD 1TB", "nothing left after cleaning: what the drive said");
    Check(CleanDriveModel(L"", L"   ") == L"Disk" && CleanDriveModel(L"", L"") == L"Disk", "nothing at all: \"Disk\"");
    Check(IsCapacityWord(L"2000GB") && IsCapacityWord(L"1TB") && IsCapacityWord(L"500G") && IsCapacityWord(L"512gb") && IsCapacityWord(L"1.92TB"),
          "capacity words: 2000GB, 1TB, 500G, 512gb, 1.92TB");
    Check(!IsCapacityWord(L"1T0") && !IsCapacityWord(L"SN850X") && !IsCapacityWord(L"TB") && !IsCapacityWord(L"1.TB") && !IsCapacityWord(L"870"),
          "not capacities: 1T0, SN850X, TB, 1.TB, 870");
}

void Smbios() {
    Table t = DesktopBoard();
    std::vector<MemoryModule> m = ParseSmbiosMemory(t.bytes.data(), t.bytes.size());
    Check(m.size() == 4, "the desktop board's table: 4 memory slots (type 17), the BIOS structure skipped");
    Check(m.size() == 4 && !m[0].installed && m[1].installed && !m[2].installed && m[3].installed,
          "...modules in the 2nd and 4th (size 0 = empty slot)");
    Check(m.size() == 4 && m[1].bank == L"P0 CHANNEL A" && m[1].device == L"DIMM 1" && m[3].bank == L"P0 CHANNEL B" && m[1].sizeMB == 32768,
          "...their Bank/Device Locators and the 32 GB from the extended size field");
    Check(InstalledModules(m) == 2, "installed modules: 2");

    Table soldered;   // a laptop: LPDDR5X on the package, "row of chips"
    soldered.Memory(16384, 0x0B, "Controller0-ChannelA", "BANK 0");
    soldered.Memory(16384, 0x0B, "Controller1-ChannelA", "BANK 1");
    soldered.End();
    m = ParseSmbiosMemory(soldered.bytes.data(), soldered.bytes.size());
    Check(m.size() == 2 && InstalledModules(m) == 0, "soldered memory (form factor row of chips): no installed modules");

    Table odd;
    odd.Memory(0xFFFF, 0x09, "DIMM_A1", "BANK 0");   // size unknown
    odd.Memory(8192, 0x0D, "ChannelA-DIMM0", "BANK 0");   // SODIMM
    odd.Memory(8192, 0x11, "CAMM", "BANK 0");             // CAMM
    odd.End();
    m = ParseSmbiosMemory(odd.bytes.data(), odd.bytes.size());
    Check(m.size() == 3 && !m[0].installed && m[1].installed && m[2].installed, "size unknown (0xFFFF): not counted; SODIMM and CAMM: counted");

    // a truncated table: what was read stands, nothing past the end is read
    Table cut = DesktopBoard();
    const size_t half = cut.bytes.size() - 60;
    m = ParseSmbiosMemory(cut.bytes.data(), half);
    Check(m.size() >= 1 && m.size() < 4, "a truncated table: the modules before the cut, no read past it");
    Check(ParseSmbiosMemory(nullptr, 0).empty() && ParseSmbiosMemory(t.bytes.data(), 3).empty(), "no table, 3 bytes: nothing");
    std::vector<uint8_t> bad = { 17, 2, 0, 0 };   // a length below the header's
    Check(ParseSmbiosMemory(bad.data(), bad.size()).empty(), "a structure claiming 2 bytes: the walk ends");
}

void Slots() {
    Check(SlotName(L"P0 CHANNEL A", L"DIMM 1") == L"A2" && SlotName(L"P0 CHANNEL B", L"DIMM 1") == L"B2" && SlotName(L"P0 CHANNEL A", L"DIMM 0") == L"A1",
          "AMD AGESA: P0 CHANNEL A + DIMM 1 -> A2 (the board's DDR5_A2), DIMM 0 -> A1");
    Check(SlotName(L"p0 channel b ", L"dimm1") == L"B2", "...any case, spaces around");
    Check(SlotName(L"BANK 0", L"Controller0-ChannelA-DIMM0") == L"A1" && SlotName(L"BANK 2", L"Controller1-ChannelA-DIMM1") == L"B2",
          "Intel DDR5: Controller<c>-ChannelA-DIMM<k> -> the controller's letter, k+1 (best effort)");
    Check(SlotName(L"BANK 0", L"ChannelA-DIMM0") == L"A1" && SlotName(L"BANK 1", L"ChannelB-DIMM1") == L"B2", "Intel DDR4: ChannelB-DIMM1 -> B2");
    Check(SlotName(L"BANK 0", L"DIMM_A1") == L"A1" && SlotName(L"", L"DDR5_B2") == L"B2", "DIMM_A1 / DDR5_B2 -> A1 / B2");
    Check(SlotName(L"BANK 0", L"DIMM0") == L"DIMM0" && SlotName(L"", L"XMM1") == L"XMM1", "a short Device Locator of another form: as it is");
    Check(SlotName(L"BANK 0", L"Some Long Locator Name").empty() && SlotName(L"", L"").empty(), "a long one or none: no slot name");
    Check(SlotName(L"BANK 0", L"Controller0-ChannelA") == L"", "\"Controller0-ChannelA\" (20 chars, no DIMM): none");

    MemoryModule a2; a2.bank = L"P0 CHANNEL A"; a2.device = L"DIMM 1";
    Check(LocatorMatches(L"P0 CHANNEL A/DIMM 1", a2) && LocatorMatches(L"p0 channel a / dimm 1", a2), "HWiNFO's (Bank/Device) matches its module, any case and spacing");
    Check(!LocatorMatches(L"P0 CHANNEL B/DIMM 1", a2) && !LocatorMatches(L"", a2) && !LocatorMatches(L"P0 CHANNEL A/DIMM 10", a2), "...and no other");
    Check(DimmLabel(1, L"A2") == L"RAM 1 · A2" && DimmLabel(3, L"") == L"RAM 3", "labels: \"RAM 1 · A2\", \"RAM 3\"");
}

void Rows() {
    Table t = DesktopBoard();
    const std::vector<MemoryModule> desk = ParseSmbiosMemory(t.bytes.data(), t.bytes.size());

    // today, HWiNFO ticked (11-13 08:20): both modules' SPD hub temperatures
    std::vector<DimmRow> r = BuildDimmRows(desk, { Temp(1, L"P0 CHANNEL A/DIMM 1", 35.8f), Temp(3, L"P0 CHANNEL B/DIMM 1", 36.0f) });
    Check(r.size() == 2 && r[0].label == L"RAM 1 · A2" && Same(r[0].temp, 35.8f) && r[1].label == L"RAM 2 · B2" && Same(r[1].temp, 36.0f),
          "the desktop board with HWiNFO's two temperatures: RAM 1 · A2 35.8, RAM 2 · B2 36.0");
    Check(CountWithTemp(r) == 2 && InstalledModules(desk) == 2, "...footer RAM 2/2");
    // HWiNFO lists B2 first: the rows keep the board's order
    r = BuildDimmRows(desk, { Temp(3, L"P0 CHANNEL B/DIMM 1", 36.0f), Temp(1, L"P0 CHANNEL A/DIMM 1", 35.8f) });
    Check(r.size() == 2 && r[0].slot == L"A2" && Same(r[0].temp, 35.8f) && r[1].slot == L"B2", "matched rows follow the board's order, not HWiNFO's");
    // nothing ticked in HWiNFO: the slots with "--"
    r = BuildDimmRows(desk, {});
    Check(r.size() == 2 && r[0].label == L"RAM 1 · A2" && std::isnan(r[0].temp) && std::isnan(r[1].temp) && CountWithTemp(r) == 0,
          "no HWiNFO temperatures: RAM 1 · A2 --, RAM 2 · B2 -- (footer RAM 0/2)");
    // only one ticked
    r = BuildDimmRows(desk, { Temp(3, L"P0 CHANNEL B/DIMM 1", 36.0f) });
    Check(r.size() == 2 && std::isnan(r[0].temp) && Same(r[1].temp, 36.0f) && CountWithTemp(r) == 1, "one of two ticked: A2 --, B2 36.0 (RAM 1/2)");
    // DDR4: no locator - rows in HWiNFO's order, no slot names, as many as the modules
    r = BuildDimmRows(desk, { Temp(0, L"", 33.0f), Temp(1, L"", 33.5f) });
    Check(r.size() == 2 && r[0].label == L"RAM 1" && r[1].label == L"RAM 2" && Same(r[0].temp, 33.0f) && Same(r[1].temp, 33.5f),
          "DDR4 (no (Bank/Device)): RAM 1, RAM 2 in HWiNFO's order, no slot names");
    r = BuildDimmRows(desk, { Temp(0, L"", 33.0f) });
    Check(r.size() == 2 && r[0].label == L"RAM 1" && Same(r[0].temp, 33.0f) && r[1].label == L"RAM 2 · A2" && std::isnan(r[1].temp),
          "...one temperature, two modules: its row, then an unmatched module without a temperature");
    // a locator that matches no module's strings: no slot name for it
    r = BuildDimmRows(desk, { Temp(1, L"BANK 0/DIMM_A2", 35.0f), Temp(3, L"P0 CHANNEL B/DIMM 1", 36.0f) });
    Check(r.size() == 2 && r[0].label == L"RAM 1" && r[1].label == L"RAM 2 · B2", "an unknown (Bank/Device): \"RAM 1\", the matched one keeps B2");
    // no modules known (SMBIOS unreadable) but HWiNFO reports: its rows; a laptop with soldered memory: none
    r = BuildDimmRows({}, { Temp(0, L"", 40.0f) });
    Check(r.size() == 1 && r[0].label == L"RAM 1", "no table but a HWiNFO temperature: one row \"RAM 1\" (the footer leaves RAM out: 0 modules)");
    Check(BuildDimmRows({}, {}).empty(), "soldered memory, nothing from HWiNFO: no rows (hidden)");
    // two modules whose strings give the same slot name: names not used
    Table same;
    same.Memory(8192, 0x09, "DIMM 0", "BANK 0");
    same.Memory(8192, 0x09, "DIMM 0", "BANK 1");
    same.End();
    const std::vector<MemoryModule> twin = ParseSmbiosMemory(same.bytes.data(), same.bytes.size());
    r = BuildDimmRows(twin, {});
    Check(r.size() == 2 && r[0].label == L"RAM 1" && r[1].label == L"RAM 2", "two modules both named \"DIMM 0\": no slot names (RAM 1, RAM 2)");

    // footer counts for the drives
    std::vector<DriveRow> drives(3);
    drives[0].temp = 39; drives[2].temp = 46;
    Check(CountWithTemp(drives) == 2, "drives: a temperature on 2 of 3 (SSD 2/3)");
    Check(Severity(NAN, 70) == 0 && Severity(59.9f, 70) == 0 && Severity(60, 70) == 1 && Severity(70, 70) == 2,
          "severity: unknown and below warm 0, 10 °C below hot 1, hot 2 (drives 60/70)");
    Check(Severity(74.9f, 85) == 0 && Severity(75, 85) == 1 && Severity(85, 85) == 2, "memory: 75 warm, 85 hot");
}

void Timing() {
    // the system drive's temperature every 5 s, from a thread that wakes every 2 s
    Period p;
    std::vector<uint64_t> at;
    for (uint64_t t = 0; t < 60000; t += 2000) if (p.Due(t, 5000)) at.push_back(t);
    Check(at.size() == 12 && at[0] == 0 && at[1] == 6000 && at[2] == 10000 && at[3] == 16000, "5 s at 2 s wakes: 0, 6, 10, 16, ... s - 12 reads a minute");
    // every second (panel open): every 5th
    Period q; int n = 0;
    for (uint64_t t = 0; t < 60000; t += 1000) n += q.Due(t, 5000);
    Check(n == 12, "5 s at 1 s wakes: 12 reads a minute");
    // wakes a little late (the 2 s wait plus scheduling): still 12 a minute, as close as the wakes allow
    Period late; n = 0;
    for (uint64_t t = 0; t < 60000; t += 2016) n += late.Due(t, 5000);
    Check(n == 12, "wakes 16 ms late: still 12 a minute");
    // a wake a few milliseconds early by the tick count's 16 ms steps: not skipped to the next one
    Period early; early.Due(0, 5000);
    Check(early.Due(4990, 5000), "a wake 10 ms before its time counts (the tick count moves in 16 ms steps)");
    // after a long gap (the PC asleep): due at once, the next a period later
    Period gap; gap.Due(0, 5000); gap.Due(5000, 5000);
    Check(gap.Due(3600000, 5000) && !gap.Due(3602000, 5000) && gap.Due(3605000, 5000), "after an hour asleep: at once, then 5 s on (no burst of catch-up reads)");
    // read by other means: the next one a period after that
    Period marked; marked.Due(0, 5000);
    marked.Mark(1000, 5000);
    Check(!marked.Due(2000, 5000) && !marked.Due(5500, 5000) && marked.Due(6000, 5000), "Mark at 1 s (the panel read it): not asked again before 6 s");
    Period held; held.Due(0, 5000);
    for (uint64_t t = 1000; t <= 60000; t += 1000) held.Mark(t, 5000);   // a minute of panel samples
    Check(!held.Due(62000, 5000) && held.Due(65000, 5000), "a minute of Marks every second (panel open), then closed: due 5 s after the last, not later");

    // the panel-open linger
    PanelLinger l;
    Check(!l.Update(false, 0) && !l.open, "no dock wants the panel: closed");
    Check(l.Update(true, 1.0) && l.open, "a dock opens it: open at once, and Update says it just opened (the sampler is woken)");
    Check(!l.Update(true, 1.1) && l.open, "still wanted: open, no second wake");
    Check(!l.Update(false, 2.0) && l.open && !l.Update(false, 6.9) && l.open, "let go: still open for 5 s");
    Check(!l.Update(false, 7.0) && !l.open, "...closed 5 s after");
    l.Update(true, 10.0); l.Update(false, 11.0);
    Check(!l.Update(true, 13.0) && l.open, "opened again within the 5 s: no new wake (it is still sampling every second)");
    Check(!l.Update(false, 14.0) && !l.Update(false, 18.9) && l.open && !l.Update(false, 19.0) && !l.open, "...and the 5 s count from the last let-go");
}

// the CPU card's name: the brand the cell's short name (hw::CpuName) leaves out put back
void CpuNames() {
    Check(PanelCpuName(L"Ryzen 7 9800X3D") == L"AMD Ryzen 7 9800X3D", "Ryzen 7 9800X3D -> AMD Ryzen 7 9800X3D");
    Check(PanelCpuName(L"Ryzen AI 9 HX 370 w/ Radeon 890M") == L"AMD Ryzen AI 9 HX 370 w/ Radeon 890M", "Ryzen AI: AMD");
    Check(PanelCpuName(L"Threadripper PRO 7995WX") == L"AMD Threadripper PRO 7995WX" && PanelCpuName(L"EPYC 9654") == L"AMD EPYC 9654", "Threadripper, EPYC: AMD");
    Check(PanelCpuName(L"i7-13700K") == L"Intel Core i7-13700K", "i7-13700K -> Intel Core i7-13700K");
    Check(PanelCpuName(L"Ultra 7 155H") == L"Intel Core Ultra 7 155H", "Core Ultra: Intel Core Ultra 7 155H");
    Check(PanelCpuName(L"12th Gen i7-1260P") == L"Intel Core i7-1260P", "12th Gen i7-1260P -> Intel Core i7-1260P (the generation is in the number)");
    Check(PanelCpuName(L"13th Gen i9-13900H") == L"Intel Core i9-13900H", "13th Gen too");
    Check(PanelCpuName(L"7 150U") == L"Intel Core 7 150U" && PanelCpuName(L"5 120U") == L"Intel Core 5 120U", "Core series 1 (\"7 150U\"): Intel Core 7 150U");
    Check(PanelCpuName(L"Xeon W-2295") == L"Intel Xeon W-2295" && PanelCpuName(L"Celeron N4020") == L"Intel Celeron N4020" &&
          PanelCpuName(L"Pentium Gold 7505") == L"Intel Pentium Gold 7505", "Xeon, Celeron, Pentium: Intel");
    Check(PanelCpuName(L"CPU") == L"CPU" && PanelCpuName(L"") == L"" && PanelCpuName(L"Snapdragon X Elite - X1E78100") == L"Snapdragon X Elite - X1E78100",
          "anything else as it is (no name: \"CPU\"; an Arm chip)");
    Check(PanelCpuName(L"12 cores") == L"12 cores" && PanelCpuName(L"9th") == L"9th", "a number that is no model: as it is");
    // the helper's reason, said short enough for the card (its cause not the part cut off)
    Check(CpuCardStatus(L"CPU 온도: 지원하지 않는 CPU이거나 PawnIO가 없습니다") == L"PawnIO 없음·미지원 CPU", "the helper's reason (sensors_main.cpp): PawnIO 없음·미지원 CPU");
    Check(CpuCardStatus(L"CPU 온도: 드라이버 오류") == L"드라이버 오류" && CpuCardStatus(L"다른 이유") == L"다른 이유", "another reason: without \"CPU 온도: \", or as it is");
}

// The panel's VRAM block: shown only for a GPU with memory of its own of a known size; the amounts from NVML,
// without it from HWiNFO; the temperature from NVML where the GPU gives one, otherwise HWiNFO's
void Vram() {
    // an 8 GB NVIDIA card as read on a test PC: NVML 1.861 of 7.960 GB, no NVML memory
    // temperature (NOT_SUPPORTED), HWiNFO's Gadget key with the memory junction temperature ticked
    const wchar_t* aero = L"dGPU [#0]: NVIDIA GeForce RTX 4060 Ti: ASUS Dual GeForce RTX 4060 Ti OC 8G (DUAL-RTX4060TI-O8G)";
    auto reading = [](int gpu, const wchar_t* sensor, float c, float usedMB = NAN, float pct = NAN) {
        GpuMemoryReading r;
        r.gpu = gpu; r.sensor = sensor; r.celsius = c; r.usedMB = usedMB; r.usagePct = pct;
        return r;
    };
    VramInput in;
    in.nvml = true; in.nvmlUsedGB = 1.861f; in.nvmlTotalGB = 7.960f; in.gpuName = L"NVIDIA GeForce RTX 4060 Ti";
    VramRow v = BuildVram(in);
    Check(v.present && Same(v.usedGB, 1.861f) && Same(v.totalGB, 7.960f) && std::fabs(v.load - 23.38f) < 0.01f && std::isnan(v.temp),
          "NVML only: 1.9 of 8.0 GB, 23 %, no temperature (\"--°C\")");
    in.hwinfo = { reading(0, aero, 44.0f) };
    v = BuildVram(in);
    Check(v.present && Same(v.temp, 44.0f) && Same(v.usedGB, 1.861f), "...with HWiNFO's memory junction temperature: 44.0 °C, the amounts still NVML's");
    in.hwinfo = { reading(0, aero, 44.0f, 9000.0f, 99.0f) };
    v = BuildVram(in);
    Check(Same(v.usedGB, 1.861f) && std::fabs(v.load - 23.38f) < 0.01f, "NVML's amounts win over HWiNFO's");
    in.nvmlTemp = 51.0f;
    v = BuildVram(in);
    Check(Same(v.temp, 51.0f), "a GPU whose NVML reports the memory temperature: NVML's, not HWiNFO's");
    in.nvmlTemp = NAN;
    // a pass NVML did not answer (the sensor thread keeps its size, the amount unknown): the block stays as it is
    in.nvmlUsedGB = NAN;
    in.hwinfo = { reading(0, aero, 44.0f) };
    v = BuildVram(in);
    Check(v.present && Same(v.totalGB, 7.960f) && std::isnan(v.usedGB) && std::isnan(v.load) && Same(v.temp, 44.0f),
          "NVML without an amount this pass: its size stays (8.0 GB, not the adapter's), \"--\" in use");
    in.hwinfo = { reading(0, aero, 44.0f, 1905.0f) };
    Check(std::fabs(BuildVram(in).usedGB - 1.860f) < 0.001f, "...or HWiNFO's amount for that pass when it reports one");
    in.nvmlUsedGB = 1.861f;

    // no memory of its own: hidden, the panel as before
    VramInput none;
    Check(!BuildVram(none).present, "no NVML, no adapter memory known: hidden");
    none.adapterGB = 0.125f; none.gpuName = L"Intel Arc 140V GPU";
    none.hwinfo = { reading(0, L"GPU [#0]: Intel Arc 140V GPU", 50.0f, 900.0f) };
    Check(!BuildVram(none).present, "an integrated GPU (128 MB of dedicated memory): hidden, whatever HWiNFO reports");
    none.adapterGB = 0.5f;
    Check(!BuildVram(none).present, "an AMD APU's 512 MB frame buffer: hidden");
    VramInput lost;
    lost.nvml = true;   // NVML is there but gave no memory size
    Check(!BuildVram(lost).present, "NVML without a memory size and no adapter memory: hidden");
    lost.adapterGB = 7.706f;
    v = BuildVram(lost);
    Check(v.present && Same(v.totalGB, 7.706f) && std::isnan(v.usedGB) && std::isnan(v.load), "...with the adapter's size: shown, the amount in use unknown (\"--\")");

    // a GPU NVML does not know (AMD, Intel): the adapter's size, HWiNFO's amount and temperature
    const wchar_t* radeon = L"GPU [#0]: AMD Radeon RX 7900 XT: Sapphire";
    VramInput amd;
    amd.adapterGB = 19.93f; amd.gpuName = L"AMD Radeon RX 7900 XT";
    v = BuildVram(amd);
    Check(v.present && Same(v.totalGB, 19.93f) && std::isnan(v.usedGB) && std::isnan(v.load) && std::isnan(v.temp),
          "no NVML, 20 GB adapter, nothing from HWiNFO: shown with \"--\" for the amount and the temperature");
    amd.hwinfo = { reading(0, radeon, 62.0f, 3412.0f) };
    v = BuildVram(amd);
    Check(v.present && std::fabs(v.usedGB - 3.332f) < 0.001f && std::fabs(v.load - 16.72f) < 0.01f && Same(v.temp, 62.0f),
          "HWiNFO's 3,412 MB: 3.3 GB (MB / 1024), 17 % of the adapter's 19.9 GB, its memory temperature 62 °C");
    amd.hwinfo = { reading(0, radeon, NAN, NAN, 40.0f) };
    v = BuildVram(amd);
    Check(std::fabs(v.usedGB - 7.972f) < 0.001f && Same(v.load, 40.0f), "only a usage in %: 40 % of the total as the amount");
    amd.hwinfo = { reading(0, radeon, NAN, 3412.0f, 40.0f) };
    Check(std::fabs(BuildVram(amd).usedGB - 3.332f) < 0.001f, "an amount and a share: the amount");
    amd.hwinfo = { reading(0, radeon, NAN, 30000.0f) };
    v = BuildVram(amd);
    Check(Same(v.load, 100.0f) && v.usedGB > 29.0f, "more in use than the adapter's size says: the load stops at 100 %");
    amd.hwinfo = { reading(0, radeon, NAN, 1927168.0f) };   // "1.9 GB" read with a bare number in MB: 1,882 GB
    v = BuildVram(amd);
    Check(v.present && std::isnan(v.usedGB) && std::isnan(v.load), "an amount far over the memory's size (more than 1.5 times) was not understood: \"--\"");
    amd.hwinfo = { reading(0, radeon, NAN, 1927168.0f, 40.0f) };
    Check(std::fabs(BuildVram(amd).usedGB - 7.972f) < 0.001f, "...the share then, when there is one");
    amd.adapterGB = 1.0f; amd.hwinfo.clear();
    Check(BuildVram(amd).present, "1 GB of dedicated memory counts as the GPU's own");

    // several GPUs in HWiNFO: the card's GPU by its name, otherwise the lowest number
    const wchar_t* arc = L"iGPU [#0]: Intel Arc Graphics";
    const wchar_t* geforce = L"dGPU [#1]: NVIDIA GeForce RTX 4060 Ti: GIGABYTE";
    in.hwinfo = { reading(0, arc, 55.0f), reading(1, geforce, 44.0f) };
    Check(Same(BuildVram(in).temp, 44.0f), "two GPUs: the sensor that names the card's GPU (#1), not the first");
    in.hwinfo = { reading(0, arc, 55.0f), reading(1, geforce, NAN, 2000.0f) };
    Check(std::isnan(BuildVram(in).temp), "the card's GPU reports no memory temperature: none - not the other GPU's");
    in.gpuName = L"GPU";
    in.hwinfo = { reading(1, geforce, 44.0f), reading(0, arc, 55.0f) };
    Check(Same(BuildVram(in).temp, 55.0f), "the GPU's name unknown (\"GPU\"): the lowest-numbered GPU with the reading");
    in.gpuName = L"NVIDIA GeForce GTX 1080";
    Check(Same(BuildVram(in).temp, 55.0f), "no sensor names the card's GPU: the lowest-numbered one as well");
    in.gpuName = L"nvidia geforce  rtx 4060 ti";
    Check(Same(BuildVram(in).temp, 44.0f), "the name matched whatever its case and spacing");
    in.hwinfo = { reading(2, geforce, 47.0f), reading(1, geforce, 44.0f) };
    Check(Same(BuildVram(in).temp, 44.0f), "two cards of the same model: the lower number");
    Check(GpuSensorNamed(aero, L"NVIDIA GeForce RTX 4060 Ti") && !GpuSensorNamed(aero, L"") && !GpuSensorNamed(aero, L"GPU") &&
          !GpuSensorNamed(arc, L"NVIDIA GeForce RTX 4060 Ti"), "a sensor is the card's when its name holds the GPU's (never for \"GPU\" or no name)");
    // ...as a whole part of the name: an RTX 4060 is not the RTX 4060 Ti, and the board's words are not the model
    const wchar_t* plain = L"dGPU [#1]: NVIDIA GeForce RTX 4060: MSI";
    Check(!GpuSensorNamed(aero, L"NVIDIA GeForce RTX 4060") && GpuSensorNamed(plain, L"NVIDIA GeForce RTX 4060") && !GpuSensorNamed(plain, L"NVIDIA GeForce RTX 4060 Ti") &&
          !GpuSensorNamed(aero, L"GeForce RTX 4060 Ti") && !GpuSensorNamed(aero, L"dGPU") && GpuSensorNamed(L"GPU [#0]: AMD Radeon RX 7900 XT", L"AMD Radeon RX 7900 XT"),
          "the model part as a whole: \"RTX 4060\" does not name an \"RTX 4060 Ti\" sensor");
    in.gpuName = L"NVIDIA GeForce RTX 4060";
    in.hwinfo = { reading(0, aero, 71.0f), reading(1, plain, 52.0f) };
    Check(Same(BuildVram(in).temp, 52.0f), "an RTX 4060 Ti (#0) and the card's RTX 4060 (#1): the RTX 4060's temperature");
    // an integrated and a discrete GPU HWiNFO both numbers 0 (two readings, hwinfo::PickGpuMemory): by the name
    in.gpuName = L"NVIDIA GeForce RTX 4060 Ti";
    in.hwinfo = { reading(0, L"dGPU [#0]: NVIDIA GeForce RTX 4060 Ti: GIGABYTE", 44.0f), reading(0, L"iGPU [#0]: AMD Radeon 780M", 66.0f) };
    Check(Same(BuildVram(in).temp, 44.0f), "\"iGPU [#0]\" beside \"dGPU [#0]\": the card's by its name");
    in.gpuName = L"GPU";
    Check(Same(BuildVram(in).temp, 44.0f), "...and with the name unknown the first of them (the discrete one comes first)");

    // the room a window keeps before the first sample
    Check(VramExpected(true, 0) && VramExpected(false, 8.0f) && VramExpected(false, 1.0f) && !VramExpected(false, 0.125f) && !VramExpected(false, 0),
          "a window keeps room for the block with an NVIDIA adapter or 1 GB of adapter memory, not for an integrated GPU");
    // its temperature's colour: warm from 90, hot at 100 (SensorClient::kVramHot)
    Check(Severity(89.9f, 100) == 0 && Severity(90, 100) == 1 && Severity(100, 100) == 2 && Severity(NAN, 100) == 0, "memory junction: 90 warm, 100 hot, unknown normal");
    PanelData empty;
    Check(!empty.vram.present && std::isnan(empty.vram.temp), "panel data starts without a VRAM block");
}

// The dock's sensor cell: what it shows of a metric (its key: the dock redraws for a sample only when it
// changes) and the text drawn from it.
void Cell() {
    const float cpuHot = 90.f, gpuHot = 83.f;
    auto key = [&](float cpuT, float cpuL, float gpuT, float gpuL, bool live = true) {
        CellKey k;
        k.live = live;
        k.cpu = MakeCellReading(cpuT, cpuL, cpuHot);
        k.gpu = MakeCellReading(gpuT, gpuL, gpuHot);
        return k;
    };
    Check(MakeCellReading(47.4f, 12, cpuHot) != MakeCellReading(47.6f, 12, cpuHot), "47.4 -> 47.6 °C: 47° -> 48°, a change");
    Check(MakeCellReading(47.2f, 12, cpuHot) == MakeCellReading(47.4f, 12, cpuHot), "47.2 -> 47.4 °C: 47° both, no change");
    {
        const CellReading a = MakeCellReading(89.6f, NAN, cpuHot), b = MakeCellReading(90.0f, NAN, cpuHot);
        Check(a.value == 90 && b.value == 90 && a.sev == 1 && b.sev == 2 && a != b,
              "89.6 and 90.0 °C: both 90°, warm and hot - a change (the colour)");
    }
    {
        const CellReading t = MakeCellReading(55, 12, cpuHot), l = MakeCellReading(NAN, 12.4f, cpuHot);
        Check(t.kind == 1 && t.value == 55 && l.kind == 2 && l.value == 12 && l.sev == 0, "a temperature wins over the load; without one the load (normal colour)");
        Check(MakeCellReading(55, 12, cpuHot) == MakeCellReading(55, 80, cpuHot), "the load changes while a temperature is known: no change");
        Check(MakeCellReading(NAN, 12.4f, cpuHot) == MakeCellReading(NAN, 11.6f, cpuHot) && MakeCellReading(NAN, 12.4f, cpuHot) != MakeCellReading(NAN, 12.6f, cpuHot),
              "only the load: 11.6 and 12.4 % both 12%, 12.6 % is 13%");
    }
    {
        const CellReading n = MakeCellReading(NAN, NAN, cpuHot);
        Check(n.kind == 0 && n.value == 0 && n.sev == 0 && n == CellReading{}, "neither known: none (\"--\")");
    }
    Check(key(NAN, NAN, NAN, NAN, false) != key(NAN, NAN, NAN, NAN, true), "not live and live: different keys (the first sample redraws)");
    Check(key(47.2f, 10, 40, 5) == key(47.4f, 30, 40.4f, 90), "the same rounded temperatures, other loads: the same key");
    Check(key(47, 10, 40, NAN) != key(47, 10, NAN, 30), "the GPU's temperature gone, its load shown: a change");
    {
        CellKey hidden = key(47, 10, NAN, NAN);
        hidden.gpuLine = false;
        Check(hidden != key(47, 10, NAN, NAN) && CellKey{}.gpuLine, "the GPU hidden (no GPU line) and a GPU shown without readings (\"GPU --\"): different keys");
    }
    Check(CellText(MakeCellReading(47.6f, 12, cpuHot)) == L"48°" && CellText(MakeCellReading(NAN, 12.4f, cpuHot)) == L"12%" &&
          CellText(MakeCellReading(NAN, 100, cpuHot)) == L"100%" && CellText(MakeCellReading(NAN, NAN, cpuHot)) == L"--" &&
          CellText(MakeCellReading(-0.4f, NAN, cpuHot)) == L"0°" && CellText(MakeCellReading(-3.6f, NAN, cpuHot)) == L"-4°",
          "the cell's text: \"48°\", \"12%\", \"100%\", \"--\" (as swprintf %d of lround drew it), below zero too");
}

}   // namespace

// The sensor settings (ShowItems) and the panel's layout from them: the heights of the approved pictures
// (PanelAll / A / B / C), the divider, the header and the footer, the room a window keeps, what fits in it.
void Settings() {
    ShowItems all;
    Check(all.source == kSourceHWiNFO && all.ReadsHWiNFO() && !all.WantsPawnIO() && all.CpuTempShown() && all.DimmsShown() &&
          all.VramTempShown(false), "the defaults: HWiNFO, everything shown, the helper not asked");
    ShowItems off = all; off.source = kSourceOff;
    Check(!off.ReadsHWiNFO() && !off.WantsPawnIO() && !off.CpuTempShown() && !off.DimmsShown() && !off.VramTempShown(false) && off.VramTempShown(true),
          "off: neither source; no CPU temperature, no module rows; the VRAM temperature row only from NVML");
    ShowItems pawn = all; pawn.source = kSourcePawnIO;
    Check(!pawn.ReadsHWiNFO() && pawn.WantsPawnIO() && pawn.CpuTempShown() && !pawn.DimmsShown() && !pawn.VramTempShown(false) && pawn.VramTempShown(true),
          "PawnIO: the helper asked, HWiNFO not read; no module rows; the VRAM temperature row only from NVML");
    pawn.cpuTemp = false;
    Check(!pawn.WantsPawnIO() && !pawn.CpuTempShown(), "PawnIO with the CPU temperature hidden: the helper not asked");
    Check(MigratedPawnIOSource(true, false) == kSourcePawnIO, "an old PawnIO switch on, PawnIO there, HWiNFO silent: PawnIO");
    Check(MigratedPawnIOSource(false, false) == kSourceHWiNFO, "...PawnIO removed since: HWiNFO Gadget (the CPU temperature does not vanish)");
    Check(MigratedPawnIOSource(true, true) == kSourceHWiNFO && MigratedPawnIOSource(false, true) == kSourceHWiNFO,
          "...HWiNFO reporting a CPU temperature (the value that dock showed): HWiNFO Gadget");
    ShowItems noVram = all; noVram.vram = false;
    Check(!noVram.VramTempShown(true), "VRAM hidden: its temperature row with it");

    // heights: 183 + 27 a module row + the VRAM block (0 / 44 / 71) + the divider (8) + 27 a drive row + 33
    Check(PanelVramDip(0) == 0 && PanelVramDip(1) == 44 && PanelVramDip(2) == 71, "the VRAM block: none, 44 DIP (line and bar), 71 (and its temperature row)");
    Check(PanelHeightDip(1, 2, 2) == 376, "all shown (one module row, VRAM with its temperature, two drive rows): 376 DIP");
    Check(PanelHeightDip(0, 2, 0) == 270, "picture A (VRAM, VRAM 온도, RAM 슬롯 온도 off): 270 DIP, no divider");
    Check(PanelHeightDip(1, 2, 0) == 305, "picture B (GPU, VRAM off): 305 DIP");
    Check(PanelHeightDip(1, 0, 2) == 314, "picture C (CPU 온도, SSD off): 314 DIP, no divider without drives");
    Check(PanelHeightDip(1, 2, 1) == 349 && PanelHeightDip(0, 1, 1) == 295, "VRAM 온도 off: the block 44 DIP (349 with a module row and two drive rows; 295 with one drive row)");
    Check(PanelHeightDip(0, 1, 0) == 243 && PanelHeightDip(0, 1, 2) == 322 && PanelHeightDip(1, 3, 2) == 403, "the heights from before (243, 322, 403) stay");
    Check(PanelDivider(1, 0, 1) && PanelDivider(0, 1, 1) && PanelDivider(0, 2, 2) && !PanelDivider(0, 0, 2) && !PanelDivider(1, 2, 0) && !PanelDivider(0, 0, 0),
          "the divider: only with drive rows below, and module rows or the VRAM block above them");

    // header and footer
    Check(PanelHeader(true, true, true) == L"CPU · GPU · RAM · VRAM · SSD" && PanelHeader(true, false, true) == L"CPU · GPU · RAM · SSD" &&
          PanelHeader(false, false, true) == L"CPU · RAM · SSD" && PanelHeader(true, true, false) == L"CPU · GPU · RAM · VRAM" &&
          PanelHeader(false, false, false) == L"CPU · RAM", "the header names what the panel shows (all, A, B, C, the least)");
    PanelFooterText f = PanelFooter(true, true, 3, 3, 0, true, 2, 2, 0);
    Check(f.counts == L"SSD 3/3 · RAM 2/2 · 1초 갱신" && f.leftOut.empty(), "the footer, all shown: \"SSD 3/3 · RAM 2/2 · 1초 갱신\"");
    f = PanelFooter(true, true, 3, 3, 0, false, 2, 0, 0);
    Check(f.counts == L"SSD 3/3 · 1초 갱신", "RAM 슬롯 온도 off (picture A), or no HWiNFO: no \"RAM a/b\"");
    f = PanelFooter(true, false, 3, 3, 1, true, 2, 2, 0);
    Check(f.counts == L"RAM 2/2 · 1초 갱신" && f.leftOut.empty(), "SSD off (picture C): no \"SSD a/b\", no \"SSD 외 N개\"");
    f = PanelFooter(true, true, 11, 9, 5, true, 4, 1, 2);
    Check(f.counts == L"SSD 9/11 · RAM 1/4 · 1초 갱신" && f.leftOut == L"SSD 외 5개 · RAM 외 2개", "what did not fit: \"SSD 외 5개 · RAM 외 2개\"");
    f = PanelFooter(true, true, 11, 9, 5, false, 4, 1, 2);
    Check(f.leftOut == L"SSD 외 5개", "...the modules hidden: no \"RAM 외 N개\"");
    f = PanelFooter(false, true, 3, 3, 0, true, 2, 2, 0);
    Check(f.counts == L"측정 중…" && f.leftOut.empty(), "before the first sample: \"측정 중…\"");

    // the room a window keeps: what is hidden keeps none
    const PanelRoom counted{ 3, 2, true, true };
    Check(ShownRoom(counted, all, false) == counted, "all shown: the room as counted");
    ShowItems a = all; a.vram = false; a.vramTemp = false; a.ramTemp = false;
    Check(ShownRoom(counted, a, false) == PanelRoom{ 3, 0, false, false }, "picture A: no module rows, no VRAM block");
    ShowItems c = all; c.cpuTemp = false; c.ssd = false;
    Check(ShownRoom(counted, c, false) == PanelRoom{ 0, 2, true, true }, "picture C: no drives");
    ShowItems vt = all; vt.vramTemp = false;
    Check(ShownRoom(counted, vt, false) == PanelRoom{ 3, 2, true, false }, "VRAM 온도 off: the block without its row");
    Check(ShownRoom(counted, off, false) == PanelRoom{ 3, 0, true, false } && ShownRoom(counted, off, true) == PanelRoom{ 3, 0, true, true },
          "off (or PawnIO): no module rows; the VRAM temperature row only once NVML gave one");
    Check(ShownRoom(PanelRoom{ 3, 2, false, false }, all, true) == PanelRoom{ 3, 2, false, false }, "no VRAM block counted: none, whatever NVML");
    // the window's room for NVML's row without HWiNFO: from the start on an NVIDIA adapter (its NVML may give one:
    // known only at the first panel sample, after the window was built), otherwise once NVML gave one
    Check(NvmlVramTempRoom(false, true) && NvmlVramTempRoom(true, false) && !NvmlVramTempRoom(false, false), "NVML's VRAM temperature row: room on an NVIDIA adapter, or once seen");
    Check(ShownRoom(counted, off, NvmlVramTempRoom(false, true)) == PanelRoom{ 3, 0, true, true } &&
          ShownRoom(counted, off, NvmlVramTempRoom(false, false)) == PanelRoom{ 3, 0, true, false },
          "off (or PawnIO) at the dock's start: the VRAM temperature row's room on an NVIDIA adapter, none on another");
    PanelRoomRows rr = RoomRows(PanelRoom{ 3, 2, true, true }, true, 2, 4, 6);
    Check(rr.dimmRows == 1 && rr.driveRows == 2 && rr.vram == 2 && PanelHeightDip(rr.dimmRows, rr.driveRows, rr.vram) == 376, "a desktop's room (one module row, VRAM with its temperature, two drives): 376 DIP");
    rr = RoomRows(PanelRoom{ 0, 0, false, false }, true, 2, 4, 6);
    Check(rr.driveRows == 1 && rr.dimmRows == 0 && rr.vram == 0, "no drive counted but drives shown: one drive row kept (as before)");
    rr = RoomRows(PanelRoom{ 0, 2, true, true }, false, 2, 4, 6);
    Check(rr.driveRows == 0 && PanelHeightDip(rr.dimmRows, rr.driveRows, rr.vram) == 314, "drives hidden: no drive row kept (picture C's room, 314)");
    rr = RoomRows(PanelRoom{ 11, 9, true, false }, true, 1, 4, 6);
    Check(rr.driveRows == 6 && rr.dimmRows == 4 && rr.vram == 1, "a vertical dock, one a row: at most six drive and four module rows; the block without its row");

    // what fits: the room of each picture holds exactly its rows
    for (auto [dimms, drives, vram] : { std::tuple{ 2, 3, 2 }, std::tuple{ 0, 3, 0 }, std::tuple{ 2, 3, 0 }, std::tuple{ 2, 0, 2 }, std::tuple{ 2, 3, 1 } }) {
        const float room = PanelHeightDip((dimms + 1) / 2, (drives + 1) / 2, vram) + 0.5f;
        const PanelFit fit = FitPanel(room, 2, drives, dimms, 4, vram, vram);
        Check(fit.dimmRows == (dimms + 1) / 2 && fit.driveRows == (drives + 1) / 2 && fit.vram == vram && fit.drivesHidden == 0 && fit.dimmsHidden == 0 &&
              fit.height == PanelHeightDip(fit.dimmRows, fit.driveRows, fit.vram), "each picture's room holds its rows, nothing left out");
    }
    {
        // more drives than the room: they take every row beside the block, the modules counted
        const PanelFit fit = FitPanel(403.5f, 2, 11, 2, 4, 2, 2);
        Check(fit.vram == 2 && fit.driveRows == 4 && fit.dimmRows == 0 && fit.drivesShown == 8 && fit.drivesHidden == 3 && fit.dimmsHidden == 2 && fit.height <= 403.5f,
              "more drives than the room: four drive rows beside the block, three drives and both modules counted");
        // the room exactly the block and its footer: no drive row (the first one would bring the divider too)
        const PanelFit tight = FitPanel(PanelHeightDip(0, 0, 2) + 20, 2, 2, 0, 4, 2, 2);
        Check(tight.driveRows == 0 && tight.drivesHidden == 2 && tight.height == PanelHeightDip(0, 0, 2), "a drive row that fits only without its divider is not squeezed in");
        // a temperature row the window was built without: only where it fits beside the rest
        PanelFit later = FitPanel(PanelHeightDip(1, 2, 1) + 0.5f, 2, 3, 2, 4, 2, 1);
        Check(later.vram == 1 && later.driveRows == 2 && later.dimmRows == 1, "the VRAM temperature row later than the window's room: left out, nothing pushed out");
        later = FitPanel(PanelHeightDip(1, 2, 1) + 0.5f, 2, 1, 0, 4, 2, 1);
        Check(later.vram == 2 && later.driveRows == 1, "...and shown where the room holds it beside everything else");
        later = FitPanel(PanelHeightDip(0, 1, 0) + 0.5f, 2, 1, 0, 4, 2, 0);
        Check(later.vram == 0, "a block the window has no room for at all: left out");
        // hidden: nothing of it, however much room
        const PanelFit none = FitPanel(1000, 2, 0, 0, 4, 0, 2);
        Check(none.vram == 0 && none.driveRows == 0 && none.dimmRows == 0 && none.height == PanelHeightDip(0, 0, 0), "everything hidden: the cards, the RAM line and the footer");
    }

    // BuildVram: whether the temperature is NVML's
    VramInput in; in.nvml = true; in.nvmlTotalGB = 16; in.nvmlUsedGB = 2;
    Check(!BuildVram(in).nvmlTemp, "NVML without a memory temperature: not NVML's");
    in.nvmlTemp = 50;
    Check(BuildVram(in).nvmlTemp && BuildVram(in).temp == 50, "NVML with one: NVML's");
}

int main() {
    Models();
    Smbios();
    Slots();
    Rows();
    Timing();
    CpuNames();
    Vram();
    Cell();
    Settings();
    return Summary("sensor_logic_test");
}
