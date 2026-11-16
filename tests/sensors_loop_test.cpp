// The elevated helper's main loop (sensors/sensors_main.cpp, Schedule): how often it wakes, when it starts and
// stops sampling (and lets the PawnIO driver go), and when it exits - driven here by a clock of its own over
// simulated hours, the dock's want event and its instance mutex as the test says. Also what its passes do with the
// sensors (Sensors::Run, with a fake driver and sampler) and CpuTemp::Close, with a fake driver library. The helper's own main is compiled but never called: nothing is installed, started,
// opened or sampled, no driver is touched.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\sensors_loop_test.cpp /link user32.lib advapi32.lib pdh.lib secur32.lib shell32.lib ole32.lib
#define wWinMain HelperMain   // the helper's entry point: compiled, not run
#include "../sensors/sensors_main.cpp"
#undef wWinMain
#include <functional>
#include "portable_check.h"

namespace {

constexpr ULONGLONG kSecond = 1000, kHour = 3600 * kSecond;

// The loop as the helper runs it: a pass whenever the schedule says (in between, the window service waits for
// requests), what the dock wants at that moment, what the pass does.
struct Sim {
    Schedule schedule;
    std::function<bool(ULONGLONG)> wanted = [](ULONGLONG) { return false; };
    std::function<bool(ULONGLONG)> dock = [](ULONGLONG) { return true; };
    std::function<void(const Schedule::Pass&)> pass = [](const Schedule::Pass&) {};   // what the pass does
    int passes = 0, samples = 0, starts = 0, stops = 0;
    ULONGLONG firstStart = 0, firstStop = 0, firstSample = 0, exitedAt = 0;
    bool exited = false;

    explicit Sim(ULONGLONG start) : schedule(start) {}

    // The passes before `until`.
    void Run(ULONGLONG until) {
        while (!exited && schedule.next < until) {
            const ULONGLONG now = schedule.next;
            const Schedule::Pass p = schedule.Step(now, wanted(now), dock(now));
            ++passes;
            pass(p);
            if (p.start && !starts++) firstStart = now;
            if (p.stop && !stops++) firstStop = now;
            if (p.sample && !samples++) firstSample = now;
            if (p.exit) { exited = true; exitedAt = now; }
        }
    }
    void Reset() { passes = samples = starts = stops = 0; firstStart = firstStop = firstSample = 0; }
};

void Wakes() {
    {   // the helper serves the windows of elevated programs; the dock never asks it for sensors
        Sim s(0);
        s.Run(kHour);
        const int first = s.passes;
        s.Reset();
        s.Run(2 * kHour);
        std::printf("  windows only: %d passes in the first hour, %d in the second (the loop of before: 3,600 each)\n", first, s.passes);
        Check(s.passes == 720, "a helper that only serves windows wakes 720 times an hour (was 3,600: once a second)");
        Check(first <= 31 + 720, "its first hour: once a second for 30 s, then every 5 s");
        Check(s.samples == 0 && s.starts == 0 && !s.exited, "it samples nothing and stays while the dock is there");
    }
    {   // CPU temperature on: one sample a second, as before
        Sim s(0);
        s.wanted = [](ULONGLONG) { return true; };
        s.Run(kHour);
        Check(s.passes == 3600 && s.samples == 3600, "while the dock wants sensors: a sample every second (3,600 an hour), as before");
        Check(s.starts == 1 && s.firstStart == 0 && s.stops == 0, "sampling starts on the first pass and stays on");
    }
}

void WantChanges() {
    {   // the dock makes its want event a little after the helper started (the dock started it first)
        Sim s(0);
        s.wanted = [](ULONGLONG t) { return t >= 12 * kSecond + 300; };
        s.Run(kHour);
        Check(s.firstSample - (12 * kSecond + 300) <= kSecond, "the dock asks 12 s after the helper started: the first sample within 1 s");
    }
    {   // the dock restarts (an update): its want event and its mutex are gone for 3 s
        Sim s(0);
        const ULONGLONG gone = 600 * kSecond, back = gone + 3 * kSecond + 400;
        s.wanted = [=](ULONGLONG t) { return t < gone || t >= back; };
        s.dock = s.wanted;
        s.Run(back);
        Check(s.stops == 1 && s.firstStop - gone <= kSecond, "the dock goes: the driver let go within 1 s");
        s.Reset();
        s.Run(back + 60 * kSecond);
        std::printf("  dock restarted: sampling again %llu ms after it asked\n", s.firstSample - back);
        Check(s.starts == 1 && s.firstSample - back <= kSecond, "it is back and asks again: the first sample within 1 s, as before");
        Check(!s.exited, "and the helper stayed");
    }
    {   // CPU temperature turned off at 10 min
        Sim s(0);
        const ULONGLONG off = 600 * kSecond + 200;
        s.wanted = [=](ULONGLONG t) { return t < off; };
        s.Run(off + 31 * kSecond);
        Check(s.stops == 1 && s.firstStop - off <= kSecond, "CPU temperature turned off: the driver let go on the next pass (within 1 s; was: when the dock exits)");
        s.Reset();
        s.Run(off + 31 * kSecond + kHour);
        Check(s.samples == 0 && s.passes == 720, "and no samples after it; 720 passes an hour once 30 s have gone by");
    }
    {   // turned on (by hand in settings.ini) while the helper has been serving windows only for a while
        Sim s(0);
        const ULONGLONG on = 600 * kSecond + 700;
        s.wanted = [=](ULONGLONG t) { return t >= on; };
        s.Run(on + 60 * kSecond);
        std::printf("  asked after 10 min of windows only: first sample %llu ms later\n", s.firstSample - on);
        Check(s.firstSample > on && s.firstSample - on <= 5 * kSecond, "the dock asks after 10 min of windows only: the first sample within 5 s");
    }
}

void Exits() {
    {   // the dock is gone (it wanted sensors): exits 30-31 s later, as before
        Sim s(0);
        const ULONGLONG gone = 600 * kSecond + 500;
        s.wanted = [=](ULONGLONG t) { return t < gone; };
        s.dock = s.wanted;
        s.Run(gone + 120 * kSecond);
        std::printf("  dock gone (with sensors): exit %llu ms later\n", s.exitedAt - gone);
        Check(s.exited && s.exitedAt - gone >= 30 * kSecond && s.exitedAt - gone <= 31 * kSecond, "the dock is gone (it wanted sensors): the helper exits 30-31 s later, as before");
    }
    {   // the dock is gone (windows only): 30-35 s
        Sim s(0);
        const ULONGLONG gone = 600 * kSecond + 500;
        s.dock = [=](ULONGLONG t) { return t < gone; };
        s.Run(gone + 120 * kSecond);
        std::printf("  dock gone (windows only): exit %llu ms later\n", s.exitedAt - gone);
        Check(s.exited && s.exitedAt - gone >= 30 * kSecond && s.exitedAt - gone <= 35 * kSecond, "the dock is gone (windows only): the helper exits 30-35 s later");
    }
    {   // whenever between two 5 s passes the dock goes: never sooner than 30 s
        ULONGLONG soonest = ~0ull, latest = 0;
        for (ULONGLONG gone = 600 * kSecond; gone < 605 * kSecond; gone += 100) {
            Sim s(0);
            s.dock = [=](ULONGLONG t) { return t < gone; };
            s.Run(gone + 120 * kSecond);
            if (!s.exited) { soonest = 0; break; }
            soonest = std::min(soonest, s.exitedAt - gone);
            latest = std::max(latest, s.exitedAt - gone);
        }
        std::printf("  dock gone at any moment (windows only): exit %llu-%llu ms later\n", soonest, latest);
        Check(soonest >= 30 * kSecond && latest <= 35 * kSecond, "the dock goes at any moment between two passes: the helper exits 30-35 s later, never sooner");
    }
    {   // the dock is back within 30 s: the helper stays
        Sim s(0);
        const ULONGLONG gone = 600 * kSecond;
        s.dock = [=](ULONGLONG t) { return t < gone || t >= gone + 20 * kSecond; };
        s.Run(gone + kHour);
        Check(!s.exited, "the dock is back within 30 s: the helper stays");
    }
}

// Sensors::Run as the helper's passes call it, with a fake driver (CpuTemp) and sampler (hw::Sampler): counted.
struct FakeCpu {
    int vendor = 0, inits = 0, closes = 0, reads = 0;
    bool open = false;
    bool Init() { ++inits; open = true; return true; }
    void Close() { ++closes; open = false; }
    bool Read(float& t) { ++reads; t = 50; return open; }
};
struct FakeSampler {
    int inits = 0, samples = 0;
    void Init() { ++inits; }
    void Sample(LDSensorBlock&) { ++samples; }
};

void Passes() {
    LDSensorBlock blk{};
    Sensors<FakeCpu, FakeSampler> sensors;
    Sim s(0);
    const ULONGLONG off = 600 * kSecond + 200, on = 1200 * kSecond + 700;
    s.wanted = [=](ULONGLONG t) { return t < off || t >= on; };
    int openSamples = 0, closedReads = 0;
    s.pass = [&](const Schedule::Pass& p) {
        const int reads = sensors.cpu.reads;
        sensors.Run(p, &blk);
        if (p.sample && sensors.cpu.open) ++openSamples;
        if (!sensors.cpu.open && sensors.cpu.reads != reads) ++closedReads;
    };
    s.Run(off);
    Check(sensors.cpu.inits == 1 && sensors.cpu.closes == 0 && sensors.haveCpu, "the dock wants sensors: the driver opened on the first pass, kept open");
    Check(sensors.sampler.inits == 1 && sensors.sampler.samples == s.samples && blk.seq == 2u * s.samples && blk.magic == LD_SENSOR_MAGIC,
          "and a sample published on every pass (its seqlock even after each)");
    const int sampled = sensors.sampler.samples;
    s.Run(off + kSecond + 1);
    Check(sensors.cpu.closes == 1 && !sensors.cpu.open && !sensors.haveCpu, "it stops wanting them: the driver let go on the next pass (pawnio_close)");
    s.Run(on);
    Check(sensors.cpu.closes == 1 && sensors.sampler.samples == sampled, "and nothing sampled nor closed again while it does not want them");
    s.Run(on + 60 * kSecond);
    Check(sensors.cpu.inits == 2 && sensors.cpu.open && sensors.haveCpu, "it wants them again: the driver opened again");
    Check(sensors.sampler.inits == 1, "the sampler (no driver: PDH, NVML) set up once only");
    Check(openSamples == s.samples && closedReads == 0 && sensors.sampler.samples == s.samples, "every sample with the driver open, none read from a closed one");
}

// CpuTemp::Close with a fake driver library: the driver handle closed once, the PCI mutex and the library let go.
int g_closes = 0;
HANDLE g_closed = nullptr;
HRESULT WINAPI FakePawnClose(HANDLE h) { ++g_closes; g_closed = h; return S_OK; }

void Close() {
    CpuTemp c;
    c.vendor = CpuTemp::Amd;
    c.h = (HANDLE)(uintptr_t)0x1234;
    c.close = FakePawnClose;
    c.pciMutex = CreateMutexW(nullptr, FALSE, nullptr);
    const bool wasLoaded = GetModuleHandleW(L"msimg32.dll") != nullptr;   // a system library this test does not use
    c.lib = LoadLibraryExW(L"msimg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    c.tjMax = 95;
    const HANDLE mutex = c.pciMutex;
    c.Close();
    Check(g_closes == 1 && g_closed == (HANDLE)(uintptr_t)0x1234 && !c.h, "CpuTemp::Close: the driver handle closed (pawnio_close)");
    DWORD flags = 0;
    Check(mutex && !c.pciMutex && !GetHandleInformation(mutex, &flags), "the PCI mutex handle closed");
    Check(!c.lib && (wasLoaded || !GetModuleHandleW(L"msimg32.dll")), "PawnIOLib let go");
    Check(c.vendor == CpuTemp::None && c.tjMax == 100 && !c.exec && !c.close, "and the rest as before Init (Init opens it all again)");
    float t = 0;
    Check(!c.Read(t), "no reading without the driver");
    c.Close();
    Check(g_closes == 1, "a second Close closes nothing again");
}

}   // namespace

int main() {
    Wakes();
    WantChanges();
    Exits();
    Passes();
    Close();
    return Summary("sensors_loop_test");
}
