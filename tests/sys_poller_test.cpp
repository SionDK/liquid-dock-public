// The background poller of src/system.cpp on Windows (or Wine): it starts, reads the battery and the
// network at once, rests between passes (no busy loop: the whole process uses next to no CPU over a few
// seconds), answers a reported interface or address change at once and reads again kNetFollow later,
// and the volume it publishes follows Publish's rule (changes only, each a new version). Touches nothing:
// it only reads.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\sys_poller_test.cpp /link user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib iphlpapi.lib advapi32.lib
#include "../src/system.cpp"
#include <cstdarg>
#include <cstdio>

HWND g_mainWnd = nullptr;
static std::atomic<int> g_logs{ 0 };
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    ++g_logs;
    char utf8[2048] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    printf("  log: %s\n", utf8);
}
double NowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}
namespace nativetray { void CacheTrayButtons(HWND) {} }
namespace taskbarstart { void Remember(HWND) {} }

static int checks = 0, failures = 0;
static void Check(bool ok, const char* what) {
    ++checks; if (!ok) ++failures;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
}
static double ProcessCpu() {
    FILETIME a, b, k, u;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    auto v = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime) / 1e7; };
    return v(k) + v(u);
}

int main() {
    SysInfo sys;
    sys.volume = 0.5f; sys.muted = false;
    int v0 = sys.version;
    Check(!sys.Publish(0.5f, false) && sys.version == v0, "Publish: the same volume and mute is no change");
    Check(sys.Publish(0.7f, false) && sys.version == v0 + 1 && std::fabs(sys.volume - 0.7f) < 1e-6, "Publish: a new level is a change (and says so)");
    Check(sys.Publish(0.7f, true) && sys.version == v0 + 2 && sys.muted, "Publish: mute is a change");
    Check(!sys.Publish(0.7005f, true) && sys.version == v0 + 2, "Publish: a level within 0.1 % of the shown one is no change (rounding of the device's steps)");
    Check(sys.Publish(0.0f, true) && sys.Publish(1.0f, false) && sys.version == v0 + 4, "Publish: the ends of the range");
    sys.Publish(0.7f, false);

    sys.net = -1;   // not a value the poller writes: shows that it read the network
    const double cpu0 = ProcessCpu();
    sys.Start();
    Sleep(1500);
    Check(sys.net >= 0 && sys.net <= 2, "the network is read on the first pass");
    {   // the battery cell: what GetSystemPowerStatus says now, read on the first pass
        SYSTEM_POWER_STATUS ps{};
        GetSystemPowerStatus(&ps);
        battery::Raw raw;
        raw.acLine = ps.ACLineStatus; raw.flag = ps.BatteryFlag; raw.percent = ps.BatteryLifePercent;
        raw.systemStatus = ps.SystemStatusFlag; raw.lifeTime = ps.BatteryLifeTime;
        const battery::State want = battery::FromRaw(raw), got = sys.Battery();
        printf("  battery: %s %d%%%s%s\n", got.present ? "present" : "none", got.percent, got.plugged ? " plugged" : "", got.charging ? " charging" : "");
        Check(got.present == want.present && (!want.present || std::abs(got.percent - want.percent) <= 1),
              "the battery is read on the first pass (a laptop: its charge; a desktop: no cell)");
    }
    const double cpu1 = ProcessCpu();
    Sleep(3000);
    const double cpu2 = ProcessCpu();
    printf("  cpu: first 1.5 s %.3f s, next 3 s %.3f s\n", cpu1 - cpu0, cpu2 - cpu1);
    Check(cpu2 - cpu1 < 0.15, "the poller rests between passes (no busy loop)");

    // How long until the poller has read the network again (sys.net set). A real machine reports network
    // changes of its own meanwhile (a VPN adapter such as Tailscale's, IPv6 addresses coming and going): a
    // read may then come early, or up to kNetGap late (a burst is read at most once a second). The bounds
    // tell a read on report from the next timed look (kNetSafety, 30 s); the times are printed.
    auto readWithin = [&](double seconds) {
        const double until = NowSeconds() + seconds;
        while (sys.net < 0 && NowSeconds() < until) Sleep(5);
        return sys.net >= 0;
    };
    const double soon = syspoll::kNetGap + 1.5;
    sys.net = -1;
    double t0 = NowSeconds();
    OnIpInterfaceChange(nullptr, nullptr, MibParameterNotification);   // as Windows calls it
    bool ok = readWithin(soon);
    printf("  read %.2f s after a reported interface change\n", NowSeconds() - t0);
    Check(ok, "a reported interface change is read at once (within kNetGap, not at the next timed look)");
    Sleep(300);
    sys.net = -1;
    t0 = NowSeconds();
    ok = readWithin(syspoll::kNetFollow + syspoll::kNetGap + 1.0);
    printf("  read again %.2f s later\n", NowSeconds() - t0);
    Check(ok, "...and read again while it settles (every kNetFollow; the address and the gateway come after the link)");
    Sleep(static_cast<DWORD>(syspoll::kNetGap * 1000) + 200);
    sys.net = -1;
    t0 = NowSeconds();
    OnAddressChange(nullptr, nullptr, MibAddInstance);
    ok = readWithin(soon);
    printf("  read %.2f s after a reported address change\n", NowSeconds() - t0);
    Check(ok, "a reported address change (DHCP done) is read at once too");

    printf("sys_poller_test: %s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", checks, failures, failures == 1 ? "" : "s");
    return failures;
}
