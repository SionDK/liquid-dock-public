// HWiNFO Gadget values: parsing, picking the CPU, memory and GPU memory (VRAM) readings and the freshness rules
// (no registry access).
#include "../src/hwinfo.h"
#include <cstdio>
#include <cmath>
using namespace hwinfo;

static int fails = 0, checks = 0;   // checks: CHECK()s run (the last line's count)
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static bool approx(float a, float b) { return std::fabs(a - b) < 0.05f; }

int main() {
    float c = 0;
    // ParseTemperature
    CHECK(ParseTemperature(L"52.4 °C", L"52.4", c) && approx(c, 52.4f));
    CHECK(ParseTemperature(L"52,4 °C", L"52,4", c) && approx(c, 52.4f));         // decimal comma
    CHECK(ParseTemperature(L"52 °C", L"", c) && approx(c, 52.0f));               // no raw value
    CHECK(ParseTemperature(L"126.3 °F", L"126.3", c) && approx(c, 52.39f));      // Fahrenheit
    CHECK(ParseTemperature(L"45°C", L"45", c) && approx(c, 45.0f));
    CHECK(ParseTemperature(L"45 ° C", L"45", c) && approx(c, 45.0f));
    CHECK(ParseTemperature(L"45 ℃", L"45", c) && approx(c, 45.0f));
    CHECK(!ParseTemperature(L"4,198 MHz", L"4198", c));                               // not a temperature
    CHECK(!ParseTemperature(L"1.250 V", L"1.250", c));
    CHECK(!ParseTemperature(L"65.2 W", L"65.2", c));
    CHECK(!ParseTemperature(L"1,234 RPM", L"1234", c));
    CHECK(!ParseTemperature(L"255 °C", L"255", c));                              // implausible
    CHECK(!ParseTemperature(L"°C", L"", c));                                     // no number
    CHECK(ParseTemperature(L"-5 °C", L"-5", c) && approx(c, -5.0f));

    // RankCpu
    CHECK(RankCpu(L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"") == 1);
    CHECK(RankCpu(L"CPU [#0]: Intel Core i5-12600KF: DTS", L"CPU Package", L"") == 2);
    CHECK(RankCpu(L"CPU [#0]: Intel Core i5-12600KF: DTS", L"Core Max", L"") == 5);
    CHECK(RankCpu(L"ASUS TUF GAMING B650-PLUS (Nuvoton NCT6799D)", L"CPU", L"") == 7);
    CHECK(RankCpu(L"GPU [#0]: NVIDIA GeForce RTX 4080 SUPER", L"GPU Temperature", L"") == -1);
    CHECK(RankCpu(L"S.M.A.R.T.: WD_BLACK SN850X", L"Drive Temperature", L"") == -1);
    CHECK(RankCpu(L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU Die (average)", L"CPU Die (average)") == 0);   // user's pick
    CHECK(RankCpu(L"Whatever", L"cpu (tctl/tdie)", L"") == 1);                        // case-insensitive

    // PickCpu: a typical Rainmeter-style list with many readings
    std::vector<Entry> list = {
        { 0, L"System: ASUS", L"Virtual Memory Committed", L"12,345 MB", L"12345" },
        { 1, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU Package Power", L"65.2 W", L"65.2" },
        { 2, L"ASUS B650 (Nuvoton NCT6799D)", L"CPU", L"48 °C", L"48" },
        { 3, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU Die (average)", L"51.0 °C", L"51.0" },
        { 5, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"52.4 °C", L"52.4" },
        { 7, L"GPU [#0]: NVIDIA GeForce RTX 4080 SUPER", L"GPU Temperature", L"44.0 °C", L"44.0" },
    };
    CHECK(PickCpu(list, L"", c) == 4 && approx(c, 52.4f));                              // Tctl/Tdie wins
    CHECK(PickCpu(list, L"CPU Die (average)", c) == 3 && approx(c, 51.0f));             // preference honoured
    CHECK(PickCpu(list, L"No Such Reading", c) == 4);                                 // unknown preference: automatic
    std::vector<Entry> mbOnly = { { 0, L"Board (Nuvoton NCT6799D)", L"CPU", L"55 °C", L"55" },
                                  { 1, L"GPU [#0]", L"GPU Temperature", L"40 °C", L"40" } };
    CHECK(PickCpu(mbOnly, L"", c) == 0 && approx(c, 55.0f));                            // fallback: board CPU sensor
    std::vector<Entry> gpuOnly = { { 0, L"GPU [#0]", L"GPU Temperature", L"40 °C", L"40" } };
    CHECK(PickCpu(gpuOnly, L"", c) == -1);                                            // never mislabel a GPU reading
    std::vector<Entry> renamed = { { 4, L"CPU [#0]: Intel Core i3-12100: DTS", L"CPU 온도", L"38 °C", L"38" } };
    CHECK(PickCpu(renamed, L"", c) == 0 && approx(c, 38.0f));                           // user-renamed label in the CPU sensor
    std::vector<Entry> empty;
    CHECK(PickCpu(empty, L"", c) == -1);

    // a HWiNFO in Korean (an Intel laptop CPU): labels in Korean
    const wchar_t* dts = L"CPU [#0]: Intel Core Ultra 7 155H: DTS";
    CHECK(RankCpu(dts, L"코어 온도", L"") == 8);                                       // Core Temperatures
    CHECK(RankCpu(dts, L"P-core 0", L"") == 8);
    CHECK(RankCpu(dts, L"TjMAX까지의 코어 거리", L"") == -1);                          // a margin, not a temperature
    CHECK(RankCpu(dts, L"Core Distance to TjMAX", L"") == -1);
    CHECK(RankCpu(L"GPU [#0]: Intel Arc 140V GPU", L"GPU 온도", L"") == -1);          // other sensors stay out
    std::vector<Entry> korean = { { 0, dts, L"TjMAX까지의 코어 거리", L"55 °C", L"55" },
                                  { 1, dts, L"코어 온도", L"47 °C", L"47" } };
    CHECK(PickCpu(korean, L"", c) == 1 && approx(c, 47.0f));                            // the only ticked reading counts
    CHECK(PickCpu(korean, L"코어 온도", c) == 1);                                       // and can be named in settings.ini
    std::vector<Entry> both = { { 0, dts, L"코어 온도", L"47 °C", L"47" },
                                { 3, dts, L"CPU Package", L"52 °C", L"52" } };
    CHECK(PickCpu(both, L"", c) == 1 && approx(c, 52.0f));                              // a known label still wins

    // ParseTemperature: the forms values come in
    CHECK(ParseTemperature(L"52.4 °C", L"52.4", c) && approx(c, 52.4f));      // no-break space before the unit
    CHECK(ParseTemperature(L"47 °c", L"47", c) && approx(c, 47.0f));               // lower-case unit
    CHECK(ParseTemperature(L"47 °C", L"N/A", c) && approx(c, 47.0f));              // a raw value that is no number: the shown one
    CHECK(ParseTemperature(L"47 °C", L"+47", c) && approx(c, 47.0f));
    CHECK(ParseTemperature(L"149.9 °C", L"149.9", c) && !ParseTemperature(L"150 °C", L"150", c));   // the plausible range ends below 150
    CHECK(ParseTemperature(L"-39.9 °C", L"-39.9", c) && !ParseTemperature(L"-40 °C", L"-40", c));
    CHECK(!ParseTemperature(L"302 °F", L"302", c));                                  // 150 °C in Fahrenheit
    CHECK(!ParseTemperature(L"47 °K", L"47", c));                                    // neither °C nor °F
    CHECK(!ParseTemperature(L"Yes", L"1", c) && !ParseTemperature(L"", L"", c));     // a throttling flag, nothing
    float kept = 12.5f;
    CHECK(!ParseTemperature(L"4,198 MHz", L"4198", kept) && kept == 12.5f);           // not a temperature: the value is left alone

    // PickCpu: a better-ranked reading without a number does not hide a usable one; equal ranks keep the first
    std::vector<Entry> noNumber = { { 0, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"N/A", L"" },
                                    { 1, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU Die (average)", L"50 °C", L"50" } };
    CHECK(PickCpu(noNumber, L"", c) == 1 && approx(c, 50.0f));
    std::vector<Entry> twins = { { 0, dts, L"P-core 0", L"44 °C", L"44" }, { 1, dts, L"P-core 1", L"46 °C", L"46" } };
    CHECK(PickCpu(twins, L"", c) == 0 && approx(c, 44.0f));
    CHECK(RankCpu(dts, L"Core Temperatures", L"") == 8);                               // English HWiNFO: the same reading
    CHECK(RankCpu(dts, L"CPU Package", L"cpu package") == 0);                          // the user's pick, any case

    // the integrated graphics, reported inside HWiNFO's CPU sensor (Intel's "Enhanced" sensor): its temperature is
    // the GPU's. Ticked next to the cores (to see it in RTSS or a Rainmeter skin), it must not become the CPU's.
    const wchar_t* enhanced = L"CPU [#0]: Intel Core i7-1165G7: Enhanced";
    std::vector<Entry> withGraphics = { { 0, L"CPU [#0]: Intel Core i7-1165G7: DTS", L"Core Temperatures", L"47 °C", L"47" },
                                        { 1, enhanced, L"CPU GT Cores (Graphics)", L"55 °C", L"55" } };
    CHECK(RankCpu(enhanced, L"CPU GT Cores (Graphics)", L"") == -1);                   // a GPU temperature, not the CPU's
    CHECK(PickCpu(withGraphics, L"", c) == 0 && approx(c, 47.0f));                      // the cores' reading, not the graphics' 55 °C
    std::vector<Entry> onlyGraphics = { { 1, enhanced, L"CPU GT Cores (Graphics)", L"55 °C", L"55" } };
    CHECK(PickCpu(onlyGraphics, L"", c) == -1);                                       // never mislabel a GPU reading (as for GPU [#0])
    CHECK(PickCpu(onlyGraphics, L"CPU GT Cores (Graphics)", c) == 0);                  // ...unless the user names it in settings.ini
    CHECK(RankCpu(L"CPU [#0]: AMD Ryzen 7 8700G: Enhanced", L"CPU GFX", L"") == -1);   // an AMD APU's graphics
    CHECK(RankCpu(enhanced, L"CPU IA Cores", L"") == 6 && RankCpu(enhanced, L"CPU SoC", L"") == 6 &&
          RankCpu(enhanced, L"Core Max", L"") == 5 && RankCpu(enhanced, L"CPU Package", L"") == 2);   // the CPU's own readings keep their ranks

    // RankDimm / PickDimms: memory module temperatures
    {
        int slot = -1;
        std::wstring loc;
        // a HWiNFO in Korean (read from a PC's Gadget key): the sensor name stays English
        // and carries the module's name before the "(Bank/Device)"; the label is translated
        const wchar_t* a2 = L"DDR5 DIMM [#1]: A-DATA Technology AX5U6000C3032G-BLARWH (P0 CHANNEL A/DIMM 1)";
        const wchar_t* b2 = L"DDR5 DIMM [#3]: A-DATA Technology AX5U6000C3032G-BLARWH (P0 CHANNEL B/DIMM 1)";
        CHECK(RankDimm(a2, L"SPD Hub 온도", slot, loc) == 1 && slot == 1 && loc == L"P0 CHANNEL A/DIMM 1");
        CHECK(RankDimm(b2, L"SPD Hub 온도", slot, loc) == 1 && slot == 3 && loc == L"P0 CHANNEL B/DIMM 1");
        CHECK(RankCpu(a2, L"SPD Hub 온도", L"") == -1);                                   // never a CPU temperature
        CHECK(RankDimm(L"DDR5 DIMM [#1] (P0 CHANNEL A/DIMM 1)", L"SPD Hub Temperature", slot, loc) == 0 && slot == 1 &&
              loc == L"P0 CHANNEL A/DIMM 1");                                             // English, without a module name
        CHECK(RankDimm(L"DDR5 DIMM [#0] (BANK 0/Controller0-ChannelA-DIMM0)", L"SPD Hub Température", slot, loc) == 1 && slot == 0 &&
              loc == L"BANK 0/Controller0-ChannelA-DIMM0");                               // French: translated label, "SPD" kept
        CHECK(RankDimm(L"DDR5 DIMM [#2] (P0 CHANNEL B/DIMM 0)", L"Temperature Sensor 0", slot, loc) == 2 && slot == 2);   // RDIMM TS0
        CHECK(RankDimm(L"DDR5 DIMM [#2] (P0 CHANNEL B/DIMM 0)", L"PMIC Temperature", slot, loc) == -1);                // the power chip
        CHECK(RankDimm(L"lpddr5 dimm [#4]", L"SPD Hub Temperature", slot, loc) == 0 && slot == 4 && loc.empty());       // any case, no locator
        CHECK(RankDimm(L"DIMM Temperature Sensor", L"DIMM[2] Temperature", slot, loc) == 3 && slot == 2 && loc.empty());  // DDR4
        CHECK(RankDimm(L"DIMM Temperature Sensor", L"Dimm [0] Temperature", slot, loc) == 3 && slot == 0);
        // motherboard probes and other memory are not modules
        CHECK(RankDimm(L"ASUS TUF GAMING B650-PLUS (Nuvoton NCT6799D)", L"DIMM", slot, loc) == -1);
        CHECK(RankDimm(L"ASUS ROG STRIX Z790-E (Nuvoton NCT6798D)", L"DIMM[0] Temperature", slot, loc) == -1);   // no "dimm" in the sensor
        CHECK(RankDimm(L"ASUS EC", L"DDR5 Temperature", slot, loc) == -1);
        CHECK(RankDimm(L"GPU [#0]: NVIDIA GeForce RTX 5080", L"GPU Memory Junction Temperature", slot, loc) == -1);
        CHECK(RankDimm(L"DDR5 DIMM [#x] (A/B)", L"SPD Hub Temperature", slot, loc) == -1);   // no module number
        CHECK(RankDimm(L"DDR DIMM [#1]", L"SPD Hub Temperature", slot, loc) == -1);          // no generation

        std::vector<Entry> mem = {
            { 0, L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"48.6 °C", L"48.6" },
            { 1, b2, L"SPD Hub 온도", L"36.0 °C", L"36.0" },                       // HWiNFO's order need not be the slots'
            { 2, a2, L"SPD Hub 온도", L"35.8 °C", L"35.8" },
            { 3, a2, L"PMIC 온도", L"41.0 °C", L"41.0" },                          // the PMIC's: not the module's
            { 4, a2, L"VDD 전압", L"1.350 V", L"1.350" },                          // not a temperature
            { 5, L"ASUS B650 (Nuvoton NCT6799D)", L"DIMM", L"30 °C", L"30" },      // a board probe
        };
        std::vector<DimmPick> picks = PickDimms(mem);
        CHECK(picks.size() == 2);
        CHECK(picks.size() == 2 && picks[0].slot == 1 && picks[0].entry == 2 && approx(picks[0].celsius, 35.8f) &&
              picks[0].locator == L"P0 CHANNEL A/DIMM 1");                                 // ordered by slot
        CHECK(picks.size() == 2 && picks[1].slot == 3 && picks[1].entry == 1 && approx(picks[1].celsius, 36.0f));
        // one per slot: the best rank, equal ranks the earlier reading
        std::vector<Entry> twice = {
            { 0, L"DDR5 DIMM [#0] (P0 CHANNEL A/DIMM 0)", L"Temperature Sensor 0", L"40 °C", L"40" },
            { 1, L"DDR5 DIMM [#0] (P0 CHANNEL A/DIMM 0)", L"SPD Hub Temperature", L"38 °C", L"38" },
            { 2, L"DDR5 DIMM [#0] (P0 CHANNEL A/DIMM 0)", L"SPD Hub Temperature", L"39 °C", L"39" },
            { 3, L"DDR5 DIMM [#0] (P0 CHANNEL A/DIMM 0)", L"SPD Hub Temperature", L"N/A", L"" },
        };
        picks = PickDimms(twice);
        CHECK(picks.size() == 1 && picks[0].entry == 1 && approx(picks[0].celsius, 38.0f));
        std::vector<Entry> noValue = { { 0, L"DDR5 DIMM [#0] (P0 CHANNEL A/DIMM 0)", L"SPD Hub Temperature", L"N/A", L"" },
                                       { 1, L"DDR5 DIMM [#0] (P0 CHANNEL A/DIMM 0)", L"Temperature Sensor 0", L"41 °C", L"41" } };
        picks = PickDimms(noValue);
        CHECK(picks.size() == 1 && picks[0].entry == 1);                                   // a better rank without a number hides nothing
        std::vector<Entry> ddr4 = { { 7, L"DIMM Temperature Sensor", L"DIMM[1] Temperature", L"33.5 °C", L"33.5" },
                                    { 8, L"DIMM Temperature Sensor", L"DIMM[0] Temperature", L"33.0 °C", L"33.0" } };
        picks = PickDimms(ddr4);
        CHECK(picks.size() == 2 && picks[0].slot == 0 && picks[1].slot == 1 && picks[0].locator.empty());
        std::vector<Entry> many;
        for (int i = 0; i < 24; ++i)
            many.push_back({ i, L"DDR5 DIMM [#" + std::to_wstring(23 - i) + L"]", L"SPD Hub Temperature", L"30 °C", L"30" });
        picks = PickDimms(many);
        CHECK(picks.size() == 16 && picks[0].slot == 0 && picks[15].slot == 15);          // 16 at most, the lowest slots
        CHECK(PickDimms(list).empty());                                                   // the CPU list above: none
    }

    // ParseAmount / RankGpuMemory / PickGpuMemory: a GPU's own memory (VRAM)
    {
        float v = 0;
        wchar_t unit = 0;
        CHECK(ParseAmount(L"3,412 MB", L"3412", v, unit) && unit == L'M' && approx(v, 3412.0f));
        CHECK(ParseAmount(L"3,412 MB", L"", v, unit) && unit == L'M' && approx(v, 3412.0f));      // no raw value: digit groups dropped
        CHECK(ParseAmount(L"1.9 GB", L"1.9", v, unit) && unit == L'M' && approx(v, 1945.6f));      // GB -> MB
        CHECK(ParseAmount(L"41.5 %", L"41.5", v, unit) && unit == L'%' && approx(v, 41.5f));
        CHECK(ParseAmount(L"41,5 %", L"", v, unit) && unit == L'%' && approx(v, 41.5f));           // decimal comma
        CHECK(ParseAmount(L"0 MB", L"0", v, unit) && approx(v, 0.0f));
        CHECK(!ParseAmount(L"44.0 °C", L"44.0", v, unit) && !ParseAmount(L"1.250 V", L"1.250", v, unit) &&
              !ParseAmount(L"512 KB", L"512", v, unit) && !ParseAmount(L"12.5 MB/s", L"12.5", v, unit) && !ParseAmount(L"", L"", v, unit));
        CHECK(!ParseAmount(L"140 %", L"140", v, unit) && !ParseAmount(L"-3 MB", L"-3", v, unit) && !ParseAmount(L"N/A MB", L"", v, unit));
        // the bare number only where it is the shown one before rounding; otherwise the shown one, whose unit was read
        CHECK(ParseAmount(L"1.9 GB", L"1.882", v, unit) && approx(v, 1927.2f));                    // more digits: the bare one
        CHECK(ParseAmount(L"1.9 GB", L"1882", v, unit) && approx(v, 1945.6f));                     // a bare number in another unit: not 1,882 GB
        CHECK(ParseAmount(L"1882 MB", L"1,882", v, unit) && approx(v, 1882.0f));                   // ...or with a digit group read as a decimal sign
        CHECK(ParseAmount(L"1 882 MB", L"", v, unit) && approx(v, 1882.0f));                       // groups set apart by a space
        CHECK(ParseAmount(L"1\u00A0234\u00A0567 MB", L"", v, unit) && approx(v, 1234567.0f));
        CHECK(ParseAmount(L"1,9 GB", L"", v, unit) && approx(v, 1945.6f));                         // a decimal comma, not 19 GB
        CHECK(ParseAmount(L"3,4 MB", L"", v, unit) && approx(v, 3.4f) && ParseAmount(L"12,345 MB", L"", v, unit) && approx(v, 12345.0f));
        CHECK(ParseAmount(L"N/A MB", L"5", v, unit) && approx(v, 5.0f));                           // no shown number: the bare one

        int gpu = -1;
        // a HWiNFO in Korean (read from a PC's Gadget key): "dGPU [#0]", the label translated
        const wchar_t* aero = L"dGPU [#0]: NVIDIA GeForce RTX 4060 Ti: ASUS Dual GeForce RTX 4060 Ti OC 8G (DUAL-RTX4060TI-O8G)";
        CHECK(RankGpuMemory(aero, L"GPU 메모리 접합 온도", L'T', gpu) == 1 && gpu == 0);
        CHECK(RankCpu(aero, L"GPU 메모리 접합 온도", L"") == -1);                                    // never the CPU's
        { int slot = 0; std::wstring loc; CHECK(RankDimm(aero, L"GPU 메모리 접합 온도", slot, loc) == -1); }   // nor a memory module's
        const wchar_t* g0 = L"GPU [#0]: NVIDIA GeForce RTX 5080: ASUS";
        CHECK(RankGpuMemory(g0, L"GPU Memory Junction Temperature", L'T', gpu) == 0 && gpu == 0);
        CHECK(RankGpuMemory(L"iGPU [#12]: Intel Arc", L"Memory Junction Temperature", L'T', gpu) == 1 && gpu == 12);
        CHECK(RankGpuMemory(g0, L"GPU Memory Temperature", L'T', gpu) == 2);                         // AMD, older NVIDIA
        CHECK(RankGpuMemory(g0, L"GPU Memory Temperature #3", L'T', gpu) == 3);                      // per-chip readings
        CHECK(RankGpuMemory(g0, L"GPU VRAM Temperature", L'T', gpu) == 3);
        // the GPU's own temperature and its hot spot are not its memory's
        CHECK(RankGpuMemory(g0, L"GPU Temperature", L'T', gpu) == -1 && RankGpuMemory(g0, L"GPU Hot Spot Temperature", L'T', gpu) == -1 &&
              RankGpuMemory(aero, L"GPU 온도", L'T', gpu) == -1 && RankGpuMemory(aero, L"GPU 핫스팟 온도", L'T', gpu) == -1);
        CHECK(RankGpuMemory(g0, L"GPU Memory Thermal Limit", L'T', gpu) == -1);                      // a threshold
        // the memory's power stage is not the memory
        CHECK(RankGpuMemory(g0, L"GPU VR Memory Temperature", L'T', gpu) == -1 && RankGpuMemory(g0, L"GPU Memory VRM Temperature", L'T', gpu) == -1 &&
              RankGpuMemory(g0, L"GPU Memory Power Stage Temperature", L'T', gpu) == -1 && RankGpuMemory(aero, L"GPU 메모리 전원부 온도", L'T', gpu) == -1);
        // other sensors' memory readings are not a GPU's
        CHECK(RankGpuMemory(L"DDR5 DIMM [#1] (P0 CHANNEL A/DIMM 1)", L"SPD Hub Temperature", L'T', gpu) == -1 &&
              RankGpuMemory(L"System: ASUS", L"Physical Memory Used", L'M', gpu) == -1 &&
              RankGpuMemory(L"CPU [#0]: Intel Core i7-1165G7: Enhanced", L"GPU Memory Temperature", L'T', gpu) == -1 &&
              RankGpuMemory(L"GPU", L"GPU Memory Temperature", L'T', gpu) == -1 && RankGpuMemory(L"", L"GPU Memory Temperature", L'T', gpu) == -1 &&
              RankGpuMemory(L"xyGPU [#0]", L"GPU Memory Temperature", L'T', gpu) == -1 && RankGpuMemory(L"GPU [#x]", L"GPU Memory Temperature", L'T', gpu) == -1);
        // amounts in use and the share
        CHECK(RankGpuMemory(g0, L"GPU Memory Allocated", L'M', gpu) == 0 && RankGpuMemory(g0, L"GPU D3D Memory Dedicated", L'M', gpu) == 1 &&
              RankGpuMemory(g0, L"GPU Memory Usage", L'M', gpu) == 2 && RankGpuMemory(g0, L"GPU Memory Usage", L'%', gpu) == 0);
        CHECK(RankGpuMemory(g0, L"GPU D3D Memory Dynamic", L'M', gpu) == -1 && RankGpuMemory(g0, L"GPU Memory Available", L'M', gpu) == -1 &&
              RankGpuMemory(g0, L"GPU Memory Controller Load", L'%', gpu) == -1 && RankGpuMemory(g0, L"GPU Core Load", L'%', gpu) == -1 &&
              RankGpuMemory(g0, L"GPU Memory Clock", L'M', gpu) == -1);                               // shared memory, free memory, the controller
        // ...whatever else their labels say: a word for "in use" does not make the shared or the free memory the GPU's own
        CHECK(RankGpuMemory(g0, L"GPU Dynamic Memory Usage", L'M', gpu) == -1 && RankGpuMemory(aero, L"GPU 동적 메모리 사용량", L'M', gpu) == -1);
        CHECK(RankGpuMemory(g0, L"GPU Shared Memory Used", L'M', gpu) == -1 && RankGpuMemory(aero, L"GPU 공유 메모리 사용량", L'%', gpu) == -1);
        CHECK(RankGpuMemory(g0, L"GPU Memory Usage (Free)", L'M', gpu) == -1 && RankGpuMemory(aero, L"GPU 메모리 여유 사용량", L'M', gpu) == -1);
        CHECK(RankGpuMemory(g0, L"GPU Memory Controller Usage", L'%', gpu) == -1 && RankGpuMemory(aero, L"GPU 메모리 컨트롤러 사용률", L'%', gpu) == -1);
        CHECK(RankGpuMemory(aero, L"GPU 메모리 할당됨", L'M', gpu) == 3 && RankGpuMemory(aero, L"GPU D3D 메모리 전용", L'M', gpu) == 4 &&
              RankGpuMemory(aero, L"GPU 메모리 사용량", L'%', gpu) == 1 && RankGpuMemory(aero, L"GPU D3D 메모리 동적", L'M', gpu) == -1 &&
              RankGpuMemory(aero, L"GPU 메모리 사용 가능", L'M', gpu) == -1);                         // translated labels, by their words

        std::vector<Entry> key = {   // a PC's Gadget key
            { 0, L"CPU [#0]: AMD Ryzen 7 7800X3D: Enhanced", L"CPU (Tctl/Tdie)", L"38.1 °C", L"38.1" },
            { 1, L"DDR5 DIMM [#0]: Samsung", L"SPD Hub 온도", L"29.2 °C", L"29.2" },
            { 2, L"DDR5 DIMM [#2]: Team Group UD5-5200 (P0 CHANNEL B/DIMM 0)", L"SPD Hub 온도", L"29.2 °C", L"29.2" },
            { 3, aero, L"GPU 메모리 접합 온도", L"44.0 °C", L"44.0" },
        };
        std::vector<int> used;
        std::vector<GpuMemPick> g = PickGpuMemory(key, &used);
        CHECK(g.size() == 1 && g[0].gpu == 0 && g[0].sensor == aero && g[0].hasTemp && approx(g[0].celsius, 44.0f) && !g[0].hasUsed && !g[0].hasUsage);
        CHECK(used.size() == 1 && used[0] == 3);
        CHECK(PickCpu(key, L"", c) == 0 && approx(c, 38.1f) && PickDimms(key).size() == 2);            // the other pickers as before
        // everything a GPU sensor can report at once: the junction temperature, not the core's or the hot spot's
        std::vector<Entry> full = {
            { 0, g0, L"GPU Temperature", L"61.0 °C", L"61.0" },
            { 1, g0, L"GPU Hot Spot Temperature", L"72.5 °C", L"72.5" },
            { 2, g0, L"GPU Memory Temperature", L"66.0 °C", L"66.0" },
            { 3, g0, L"GPU Memory Junction Temperature", L"70.0 °C", L"70.0" },
            { 4, g0, L"GPU Memory Usage", L"38.4 %", L"38.4" },
            { 5, g0, L"GPU D3D Memory Dedicated", L"5,900 MB", L"5900" },
            { 6, g0, L"GPU Memory Allocated", L"6,144 MB", L"6144" },
            { 7, g0, L"GPU D3D Memory Dynamic", L"812 MB", L"812" },
            { 8, g0, L"GPU Memory Available", L"10,240 MB", L"10240" },
            { 9, g0, L"GPU Core Load", L"97.0 %", L"97.0" },
            { 10, g0, L"GPU Memory Controller Load", L"12.0 %", L"12.0" },
        };
        g = PickGpuMemory(full, &used);
        CHECK(g.size() == 1 && g[0].hasTemp && approx(g[0].celsius, 70.0f) && g[0].hasUsed && approx(g[0].usedMB, 6144.0f) &&
              g[0].hasUsage && approx(g[0].usagePct, 38.4f));
        CHECK(used == std::vector<int>({ 2, 3, 4, 5, 6 }));                                           // what a reader caches
        // AMD: "GPU Memory Usage" is an amount there
        std::vector<Entry> amd = { { 0, L"GPU [#0]: AMD Radeon RX 7900 XT: Sapphire", L"GPU Memory Usage", L"3,412 MB", L"3412" },
                                   { 1, L"GPU [#0]: AMD Radeon RX 7900 XT: Sapphire", L"GPU Memory Temperature", L"62.0 °C", L"62.0" } };
        g = PickGpuMemory(amd);
        CHECK(g.size() == 1 && g[0].hasUsed && approx(g[0].usedMB, 3412.0f) && !g[0].hasUsage && g[0].hasTemp && approx(g[0].celsius, 62.0f));
        // per-chip temperatures of one rank: the hottest; a better rank wins whatever its value
        std::vector<Entry> chips = { { 0, g0, L"GPU Memory Temperature #1", L"58.0 °C", L"58.0" },
                                     { 1, g0, L"GPU Memory Temperature #2", L"63.0 °C", L"63.0" },
                                     { 2, g0, L"GPU Memory Temperature #3", L"60.0 °C", L"60.0" } };
        g = PickGpuMemory(chips);
        CHECK(g.size() == 1 && approx(g[0].celsius, 63.0f));
        chips.push_back({ 3, g0, L"GPU Memory Junction Temperature", L"61.0 °C", L"61.0" });
        g = PickGpuMemory(chips);
        CHECK(g.size() == 1 && approx(g[0].celsius, 61.0f));
        // a value that is no number hides nothing; several GPUs come in their order, four at most
        std::vector<Entry> two = { { 0, L"dGPU [#1]: NVIDIA GeForce RTX 4060 Ti", L"GPU Memory Junction Temperature", L"N/A", L"" },
                                   { 1, L"dGPU [#1]: NVIDIA GeForce RTX 4060 Ti", L"GPU Memory Temperature", L"50.0 °C", L"50.0" },
                                   { 2, L"iGPU [#0]: AMD Radeon 780M", L"GPU D3D Memory Dedicated", L"420 MB", L"420" } };
        g = PickGpuMemory(two, &used);
        CHECK(g.size() == 2 && g[0].gpu == 0 && g[0].hasUsed && !g[0].hasTemp && g[1].gpu == 1 && g[1].hasTemp && approx(g[1].celsius, 50.0f));
        CHECK(used == std::vector<int>({ 1, 2 }));
        // an integrated and a discrete GPU with one number: two GPUs, the discrete one first, nothing of one in the other
        std::vector<Entry> same = { { 0, L"iGPU [#0]: AMD Radeon 780M", L"GPU D3D Memory Dedicated", L"300 MB", L"300" },
                                    { 1, L"dGPU [#0]: AMD Radeon RX 7900 XT: Sapphire", L"GPU Memory Temperature", L"40.0 °C", L"40.0" } };
        g = PickGpuMemory(same, &used);
        CHECK(g.size() == 2 && g[0].gpu == 0 && g[0].sensor == same[1].sensor && g[0].hasTemp && approx(g[0].celsius, 40.0f) && !g[0].hasUsed &&
              g[1].gpu == 0 && g[1].sensor == same[0].sensor && g[1].hasUsed && approx(g[1].usedMB, 300.0f) && !g[1].hasTemp);
        CHECK(used == std::vector<int>({ 0, 1 }));
        // two sensors of one GPU (the same letter and number) stay one pick
        std::vector<Entry> split = { { 0, g0, L"GPU Memory Allocated", L"6,144 MB", L"6144" },
                                     { 1, L"GPU [#0]: NVIDIA GeForce RTX 5080: uP9512", L"GPU Memory Junction Temperature", L"70.0 °C", L"70.0" } };
        g = PickGpuMemory(split);
        CHECK(g.size() == 1 && g[0].sensor == g0 && g[0].hasUsed && g[0].hasTemp && approx(g[0].celsius, 70.0f));
        // a sleeping GPU's 0 °C is no temperature: left out (its reading stays among those a reader follows), another one taken
        std::vector<Entry> asleep = { { 0, g0, L"GPU Memory Junction Temperature", L"0.0 °C", L"0.0" } };
        g = PickGpuMemory(asleep, &used);
        CHECK(g.size() == 1 && !g[0].hasTemp && used == std::vector<int>({ 0 }));
        asleep.push_back({ 1, g0, L"GPU Memory Temperature", L"41.0 °C", L"41.0" });
        g = PickGpuMemory(asleep);
        CHECK(g.size() == 1 && g[0].hasTemp && approx(g[0].celsius, 41.0f));
        std::vector<Entry> many;
        for (int i = 0; i < 40; ++i)
            many.push_back({ i, L"GPU [#" + std::to_wstring(39 - i) + L"]", L"GPU Memory Temperature", L"40 °C", L"40" });
        g = PickGpuMemory(many, &used);
        CHECK(g.size() == 4 && (int)used.size() == kMaxGpuMemoryEntries && g[0].gpu == 16 && g[3].gpu == 19);   // the first 24 readings looked at
        CHECK(PickGpuMemory(list).empty() && PickGpuMemory({}).empty());                             // the CPU list above (its "GPU Temperature"): none
    }

    // ParsePower: watts from the shown value, its bare number where it is that value before rounding
    {
        float w = 0;
        CHECK(ParsePower(L"71.234 W", L"71.234", w) && approx(w, 71.234f));
        CHECK(ParsePower(L"71,2 W", L"71,2", w) && approx(w, 71.2f));                 // decimal comma
        CHECK(ParsePower(L"71W", L"", w) && approx(w, 71.0f));                        // no space, no raw value
        CHECK(ParsePower(L"287.0 W ", L"286.96", w) && approx(w, 286.96f));           // the bare number before rounding
        CHECK(ParsePower(L"287 W", L"0.287", w) && approx(w, 287.0f));                // a bare number in another unit: the shown one
        CHECK(ParsePower(L"850 mW", L"850", w) && approx(w, 0.85f));
        CHECK(ParsePower(L"1.2 kW", L"1.2", w) && approx(w, 1200.0f));
        CHECK(ParsePower(L"0.0 W", L"0.0", w) && approx(w, 0.0f));                    // an idle reading is a reading
        CHECK(ParsePower(L"1 234 W", L"", w) && approx(w, 1234.0f));                  // a space between digit groups
        CHECK(!ParsePower(L"52.4 °C", L"52.4", w));                                  // a temperature
        CHECK(!ParsePower(L"45 %", L"45", w));                                       // CPU PPT Limit, a share
        CHECK(!ParsePower(L"12.3 Wh", L"12.3", w));                                  // energy
        CHECK(!ParsePower(L"4,198 MHz", L"4198", w) && !ParsePower(L"1.250 V", L"1.250", w) && !ParsePower(L"98.0 A", L"98", w));
        CHECK(!ParsePower(L"W", L"", w) && !ParsePower(L"", L"", w) && !ParsePower(L"Yes W", L"", w));
        CHECK(!ParsePower(L"9000 W", L"9000", w));                                   // implausible
        CHECK(!ParsePower(L"-5 W", L"-5", w));
    }

    // RankCpuPower / PickCpuPower: the CPU's package power, English and translated labels
    {
        const wchar_t* amd = L"CPU [#0]: AMD Ryzen 7 9800X3D: Enhanced";
        const wchar_t* intel = L"CPU [#0]: Intel Core i5-12600KF: Enhanced";
        const wchar_t* laptop = L"CPU [#0]: Intel Core Ultra 7 155H: Enhanced";
        CHECK(RankCpuPower(amd, L"CPU Package Power") == 0 && RankCpuPower(intel, L"CPU Package Power") == 0);
        CHECK(RankCpuPower(amd, L"CPU PPT") == 1);
        CHECK(RankCpuPower(amd, L"CPU 패키지 전력") == 2 && RankCpuPower(laptop, L"CPU 패키지 전원") == 2 && RankCpuPower(intel, L"Package Power") == 2);
        // the label a test PC's Korean HWiNFO gives (seen 2026-11-22)
        CHECK(RankCpuPower(amd, L"CPU 패키지 전력 소비") == 2);
        CHECK(RankCpuPower(amd, L"CPU PPT (W)") == 3);
        CHECK(RankCpuPower(L"cpu [#0]: amd", L"cpu package power") == 0);           // case-insensitive
        // limits, currents and parts of the package: never
        for (const wchar_t* l : { L"CPU PPT Limit", L"CPU 패키지 전력 한계", L"PL1 Power Limit (Static)", L"CPU 패키지 전력 제한", L"CPU TDC", L"CPU EDC",
                                  L"IA Cores Power", L"GT Cores Power", L"CPU GT Cores Power (Graphics)", L"CPU 그래픽 패키지 전력", L"GPU Package Power",
                                  L"CPU SoC Power (SVI3 TFN)", L"CPU Core Power (SVI3 TFN)", L"Core 0 Power", L"코어 0 전력", L"CPU 코어 전력",
                                  L"System Agent Power", L"Rest-of-Chip Power", L"DRAM Power", L"Memory Package Power", L"Uncore Power",
                                  L"Total System Power", L"CPU Package", L"CPU (Tctl/Tdie)" })
            CHECK(RankCpuPower(amd, l) < 0);
        // only HWiNFO's own CPU sensors: not a GPU's, not the board's, not a drive's
        CHECK(RankCpuPower(L"dGPU [#0]: NVIDIA GeForce RTX 5080", L"CPU Package Power") < 0);
        CHECK(RankCpuPower(L"ASUS TUF GAMING B650-PLUS (Nuvoton NCT6799D)", L"CPU Package Power") < 0);
        CHECK(RankCpuPower(L"S.M.A.R.T.: WD_BLACK SN850X", L"CPU Package Power") < 0);

        float w = 0;
        std::vector<Entry> amdKey = { { 0, amd, L"CPU (Tctl/Tdie)", L"46.0 °C", L"46.0" },
                                      { 1, amd, L"CPU PPT Limit", L"44.1 %", L"44.1" },
                                      { 2, amd, L"CPU PPT", L"71.5 W", L"71.512" },
                                      { 3, amd, L"CPU Core Power (SVI3 TFN)", L"40.1 W", L"40.1" },
                                      { 4, amd, L"CPU Package Power", L"69.8 W", L"69.81" } };
        CHECK(PickCpuPower(amdKey, w) == 4 && approx(w, 69.81f));                    // Package Power before PPT, never the cores'
        amdKey.pop_back();
        CHECK(PickCpuPower(amdKey, w) == 2 && approx(w, 71.512f));                   // ...PPT without it
        std::vector<Entry> korean = { { 0, laptop, L"코어 온도", L"42 °C", L"42" },
                                      { 1, laptop, L"IA 코어 전력", L"3.1 W", L"3.1" },
                                      { 2, laptop, L"CPU 패키지 전력", L"6.4 W", L"6.42" } };
        CHECK(PickCpuPower(korean, w) == 2 && approx(w, 6.42f));                     // a Korean HWiNFO: by the label's words
        std::vector<Entry> temps = { { 0, amd, L"CPU (Tctl/Tdie)", L"46.0 °C", L"46.0" },
                                     { 1, L"dGPU [#0]: NVIDIA GeForce RTX 5080", L"GPU 메모리 접합 온도", L"50.0 °C", L"50.0" } };
        w = -1;
        CHECK(PickCpuPower(temps, w) == -1 && w == -1);                              // a test PC's key (11-22): no power reported
        std::vector<Entry> wrongUnit = { { 0, amd, L"CPU Package Power", L"44 %", L"44" } };
        CHECK(PickCpuPower(wrongUnit, w) == -1);                                     // the label right, the value no power
        CHECK(PickCpuPower({}, w) == -1);
    }

    // Judge: freshness of the Gadget key
    int asked = 0;
    auto proc = [&](bool up) { return [&asked, up] { ++asked; return up; }; };
    Freshness f = Judge(12, 2.0, proc(false));
    CHECK(f.running && asked == 0);                             // fresh key: no process lookup at all
    f = Judge(12, 30.0, proc(false));
    CHECK(!f.running && asked == 1);                            // left behind by a closed HWiNFO
    f = Judge(12, 30.0, proc(true));
    CHECK(f.running);                                           // quiet for a moment, HWiNFO still runs
    f = Judge(0, 1.0, proc(true));
    CHECK(!f.running);                                          // key exists but nothing is reported (reporting switched off)
    asked = 0;
    f = Judge(12, 19.9, proc(false));
    CHECK(f.running && asked == 0);                             // the last write within 20 s: HWiNFO is taken as running, unasked
    f = Judge(12, 20.0, proc(false));
    CHECK(!f.running && asked == 1);                            // from 20 s on the process check decides
    // HWiNFO 8.54 writes a value only when it changes: a temperature steady for a minute
    // or an hour leaves the key quiet that long. While HWiNFO runs it is still its value.
    f = Judge(12, 60.5, proc(true));
    CHECK(f.running);
    f = Judge(12, 3600.0, proc(true));
    CHECK(f.running);
    f = Judge(12, 3600.0, proc(false));
    CHECK(!f.running);                                          // ...and nothing once HWiNFO is gone

    std::printf(fails ? "%d FAILED\n" : "all hwinfo tests passed\n", fails);
    std::printf("hwinfo_test: %s (%d checks, %d failure%s)\n", fails ? "FAILED" : "ALL PASSED", checks, fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
