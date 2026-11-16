// System state of src/system.cpp that the poller and the UI thread share, with the Windows calls faked
// where Windows cannot be made to answer as needed: the Wi-Fi / Ethernet test of the network button
// (HasWifiUp) reads the adapter list of this PC as the reference, then fake lists - a list longer than
// the first 16 KB guess, one that grows between the size query and the read, sized as documented and as
// Windows does it - and the battery cell's state (SetBattery / Battery) is read while another thread sets
// it, as the UI thread renders while the poller or WM_POWERBROADCAST sets it; a poller read held up
// between GetSystemPowerStatus and its store does not hide the UI thread's newer one. Touches nothing:
// it only reads.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\system_state_test.cpp /link user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib iphlpapi.lib advapi32.lib
// system.cpp's own first includes, so that only its call - not the SDK's declaration - is renamed
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
static ULONG WINAPI FakeGetAdaptersAddresses(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES out, PULONG size);
static BOOL WINAPI FakeGetSystemPowerStatus(LPSYSTEM_POWER_STATUS ps);
#define GetAdaptersAddresses FakeGetAdaptersAddresses
#define GetSystemPowerStatus FakeGetSystemPowerStatus
#include "../src/system.cpp"
#undef GetAdaptersAddresses
#undef GetSystemPowerStatus
#include <cstdarg>
#include <cstdio>

HWND g_mainWnd = nullptr;
static int g_logs = 0;
void Log(const wchar_t* fmt, ...) {
    ++g_logs;
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
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

// ---------------------------------------------------------------- the adapter list
namespace {

ULONG RealGaa(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES out, PULONG size) {
    return GetAdaptersAddresses(family, flags, reserved, out, size);
}

struct FakeList {
    bool real = true;                // pass the calls to Windows
    ULONG needSkip = 0, needFull = 0;   // bytes the list takes with the multicast / anycast / DNS entries skipped, and with them
    ULONG growAfterFirst = 0;        // bytes an address that comes between the first and the second call adds
    bool growsEveryCall = false;     // ...and again before every later call
    bool windowsSize = false;        // the size given back as Windows gives it (twice what is needed, 16383 at least), not the exact one
    ULONG type = IF_TYPE_IEEE80211;  // the one adapter that is up, with a gateway
    int calls = 0;
} g_list;

}   // namespace

static ULONG WINAPI FakeGetAdaptersAddresses(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES out, PULONG size) {
    if (g_list.real) return RealGaa(family, flags, reserved, out, size);
    ++g_list.calls;
    ULONG need = (flags & GAA_FLAG_SKIP_MULTICAST) ? g_list.needSkip : g_list.needFull;
    if (g_list.calls > 1) need += g_list.growAfterFirst * (g_list.growsEveryCall ? g_list.calls - 1 : 1);
    if (!out || *size < need) { *size = g_list.windowsSize ? std::max(16383ul, 2 * need) : need; return ERROR_BUFFER_OVERFLOW; }
    ZeroMemory(out, need);
    out->Length = sizeof(*out);
    out->IfType = g_list.type;
    out->OperStatus = IfOperStatusUp;
    out->FirstGatewayAddress = reinterpret_cast<PIP_ADAPTER_GATEWAY_ADDRESS_LH>(reinterpret_cast<BYTE*>(out) + sizeof(*out));
    return NO_ERROR;
}

// ---------------------------------------------------------------- the power status
namespace {

struct FakePower {
    std::atomic<bool> real{ true };       // pass the calls to Windows
    SYSTEM_POWER_STATUS answer{};
    std::atomic<bool> holdNext{ false };  // the next call answers, then waits for `release` (a thread held up before its store)
    HANDLE answered = CreateEventW(nullptr, TRUE, FALSE, nullptr), release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
} g_power;

}   // namespace

static BOOL WINAPI FakeGetSystemPowerStatus(LPSYSTEM_POWER_STATUS ps) {
    if (g_power.real) return GetSystemPowerStatus(ps);
    *ps = g_power.answer;
    if (g_power.holdNext.exchange(false)) {
        SetEvent(g_power.answered);
        WaitForSingleObject(g_power.release, 5000);
    }
    return TRUE;
}

namespace {

// What HasWifiUp should answer: the whole list (one generous buffer, read until it fits), same rule.
bool ReferenceWifiUp() {
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG len = 256 * 1024;
    std::vector<BYTE> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 4 && r == ERROR_BUFFER_OVERFLOW; ++i) {
        buf.resize(len);
        r = RealGaa(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &len);
    }
    if (r != NO_ERROR) return false;
    bool wifi = false, wired = false;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress) continue;
        if (a->IfType == IF_TYPE_IEEE80211) wifi = true; else if (a->IfType == IF_TYPE_ETHERNET_CSMACD) wired = true;
    }
    return wifi && !wired;
}

bool FakeWifi(ULONG needSkip, ULONG needFull, ULONG grow = 0, ULONG type = IF_TYPE_IEEE80211, bool windowsSize = false,
              bool growsEveryCall = false) {
    g_list = {};
    g_list.real = false;
    g_list.needSkip = needSkip; g_list.needFull = needFull; g_list.growAfterFirst = grow; g_list.type = type;
    g_list.windowsSize = windowsSize; g_list.growsEveryCall = growsEveryCall;
    const bool up = HasWifiUp();
    g_list.real = true;
    return up;
}

}   // namespace

int main() {
    // ---- this PC's adapters
    {
        const ULONG skip = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG a = 0, b = 0;
        RealGaa(AF_UNSPEC, skip, nullptr, nullptr, &a);
        RealGaa(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, nullptr, &b);
        printf("  this PC: Windows asks for %lu bytes for the list HasWifiUp reads, %lu with multicast, anycast and DNS entries (it starts with 16384)\n", a, b);
        const bool up = HasWifiUp(), ref = ReferenceWifiUp();
        printf("  this PC: Wi-Fi only %s\n", up ? "yes" : "no");
        Check(up == ref, "this PC's adapters: the network button's Wi-Fi test agrees with a read of the whole list");
    }
    // ---- fake lists
    Check(FakeWifi(6000, 10000), "a few adapters (fits the first 16 KB): Wi-Fi up with a gateway is Wi-Fi");
    Check(!FakeWifi(6000, 10000, 0, IF_TYPE_ETHERNET_CSMACD), "...an Ethernet adapter is not");
    // More adapters than fit 16 KB (Hyper-V and WSL switches, VPNs, virtual machines' adapters): the second
    // call asked again WITHOUT the skip flags of the first - multicast, anycast and DNS entries included -
    // into a buffer sized for the list without them, overflowed again, and the button showed Ethernet.
    // As documented, the size given back is the size needed:
    Check(FakeWifi(20000, 24000), "a list over 16 KB: still read (the second call asks for the same entries the size was given for)");
    Check(FakeWifi(17000, 17000, 200), "an address that comes between the size query and the read: read again, not taken as no Wi-Fi");
    // Windows gives more room than that - twice what is needed, 16383 at least (a 25H2 PC, 2026-10-28:
    // 5504 needed -> 16383, 9728 with those entries -> 19456; 31632 -> 63264 with every interface) - so it
    // takes adapters whose multicast / anycast / DNS entries more than double their size (Tailscale's: 816 ->
    // 2160 bytes, measured) to overflow that:
    Check(FakeWifi(17000, 40000, 0, IF_TYPE_IEEE80211, true),
          "sized as Windows does it, a list over 16 KB that the skipped entries would more than double: still read");
    Check(!FakeWifi(17000, 17000, 4000, IF_TYPE_IEEE80211, false, true) && g_list.calls == 3,
          "a list that grows before every call: given up after 3 calls (read again at the next look), no loop");

    // ---- the battery cell read while it is set
    {
        SysInfo bat;
        const battery::State onBattery{ true, 87, false, false, false, 11520 };
        const battery::State charging{ true, 12, true, true, true, -1 };
        std::atomic<bool> stop{ false };
        std::atomic<long long> reads{ 0 }, mixed{ 0 };
        std::thread writer([&] { while (!stop) { bat.SetBattery(onBattery); bat.SetBattery(charging); } });
        std::thread reader([&] {
            while (!stop) {
                const battery::State s = bat.Battery();
                ++reads;
                if (s.present && !(s == onBattery) && !(s == charging)) ++mixed;
            }
        });
        Sleep(700);
        stop = true;
        writer.join(); reader.join();
        printf("  %lld reads while it changed, %lld of them a mix of the two states\n", reads.load(), mixed.load());
        // SetBattery stored percent, flags and time left one by one, Battery read them one by one: a frame drawn
        // meanwhile showed the new charge with the old charging / saver flags (12 % on battery: the red cell).
        Check(mixed == 0, "the battery cell read while the poller sets it: always one whole state, never a mix");
        const battery::State none{};
        bat.SetBattery(none);
        Check(!bat.Battery().present, "no battery set: no cell");

        // every state FromRaw makes reads back whole: the ends of the charge and of the estimate, every flag
        bool whole = true;
        for (int pct : { 0, 1, 99, 100 })
            for (int secs : { -1, 0, 59, 7 * 24 * 3600 - 1 })
                for (int f = 0; f < 8; ++f) {
                    const battery::State s{ true, pct, (f & 1) != 0, (f & 2) != 0, (f & 4) != 0, secs };
                    bat.SetBattery(s);
                    if (!(bat.Battery() == s)) whole = false;
                }
        const battery::State gone{ false, 50, true, true, false, 600 };
        bat.SetBattery(gone);
        Check(whole && bat.Battery() == battery::State{}, "every charge, estimate and flag reads back as set; no battery reads back as none");
        const int v0 = bat.version;
        bat.SetBattery(onBattery);
        bat.SetBattery(onBattery);
        Check(bat.version == v0 + 1, "the same state set again is no change (one version bump)");
    }

    // ---- the poller and WM_POWERBROADCAST read the battery at the same time
    {
        // The poller asks Windows just before the charger is plugged in, and is held up before it stores
        // that; meanwhile the UI thread handles WM_POWERBROADCAST, reads "plugged in" and stores it. The
        // poller's older read then landed on top: the cell showed it on battery (and its colour) until the
        // next poll, 5 s later.
        SysInfo bat;
        SYSTEM_POWER_STATUS onBattery{}, plugged{};
        onBattery.ACLineStatus = 0; onBattery.BatteryFlag = 1; onBattery.BatteryLifePercent = 18; onBattery.BatteryLifeTime = 2400;
        plugged.ACLineStatus = 1; plugged.BatteryFlag = 8; plugged.BatteryLifePercent = 18; plugged.BatteryLifeTime = 0xFFFFFFFF;
        auto state = [](const SYSTEM_POWER_STATUS& ps) {
            battery::Raw raw;
            raw.acLine = ps.ACLineStatus; raw.flag = ps.BatteryFlag; raw.percent = ps.BatteryLifePercent;
            raw.systemStatus = ps.SystemStatusFlag; raw.lifeTime = ps.BatteryLifeTime;
            return battery::FromRaw(raw);
        };
        g_logs = 0;
        g_power.answer = onBattery;
        g_power.holdNext = true;
        g_power.real = false;
        std::thread poller([&] { bat.PollBattery(); });
        const bool held = WaitForSingleObject(g_power.answered, 5000) == WAIT_OBJECT_0;
        g_power.answer = plugged;
        bat.PollBattery();                          // WM_POWERBROADCAST, on the UI thread: it does not wait for the poller
        const bool uiStored = bat.Battery() == state(plugged);
        SetEvent(g_power.release);
        poller.join();
        Check(held && uiStored && bat.Battery() == state(plugged),
              "a poller read taken before the charger was plugged in, stored after the UI thread's newer read: the newer one stays");
        Check(g_logs == 1, "...and the battery is logged as present once");
        g_power.answer = onBattery;
        bat.PollBattery();
        Check(bat.Battery() == state(onBattery), "...a later read still replaces it (unplugged again)");
        g_power.real = true;
    }

    printf("system_state_test: %s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", checks, failures, failures == 1 ? "" : "s");
    return failures;
}
