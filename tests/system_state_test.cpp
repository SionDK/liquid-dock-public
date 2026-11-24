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
// system.h's own headers first, then system.h with its private members open (the battery's tickets): only its text
#include "../src/common.h"
#include "../src/battery.h"
#include <atomic>
#include <mutex>
#define private public
#include "../src/system.h"
#undef private
static ULONG WINAPI FakeGetAdaptersAddresses(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES out, PULONG size);
static BOOL WINAPI FakeGetSystemPowerStatus(LPSYSTEM_POWER_STATUS ps);
static void WINAPI FakeGetLocalTime(LPSYSTEMTIME st);
static int WINAPI FakeGetTimeFormatEx(LPCWSTR locale, DWORD flags, const SYSTEMTIME* st, LPCWSTR format, LPWSTR out, int size);
static int WINAPI FakeGetDateFormatEx(LPCWSTR locale, DWORD flags, const SYSTEMTIME* st, LPCWSTR format, LPWSTR out, int size, LPCWSTR calendar);
// SysInfo::Start's poller run on this thread with no Windows service: the COM objects are the test's own,
// the network reports are not registered, and the wait between passes is the test's (it moves the clock,
// reports changes, and ends the run by throwing)
static HRESULT WINAPI FakeCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* out);
static HRESULT WINAPI FakeCoInitializeEx(LPVOID reserved, DWORD flags);
static DWORD WINAPI FakeNotifyIpInterfaceChange(ADDRESS_FAMILY family, PIPINTERFACE_CHANGE_CALLBACK cb, PVOID context, BOOLEAN initial, HANDLE* out);
static DWORD WINAPI FakeNotifyUnicastIpAddressChange(ADDRESS_FAMILY family, PUNICAST_IPADDRESS_CHANGE_CALLBACK cb, PVOID context, BOOLEAN initial, HANDLE* out);
static DWORD WINAPI FakeWaitForSingleObject(HANDLE h, DWORD ms);
// the shell input helpers (SendKeyCombo, TakeForegroundRight, IsShellPopupWindow): no input is
// sent, no process opened - the answers are the test's; system.cpp's other headers first, so only its calls are renamed
static UINT WINAPI FakeSendInput(UINT count, LPINPUT inputs, int size);
static BOOL WINAPI FakeAllowSetForegroundWindow(DWORD pid);
static DWORD WINAPI FakeGetWindowThreadProcessId(HWND w, LPDWORD pid);
static HANDLE WINAPI FakeOpenProcess(DWORD access, BOOL inherit, DWORD pid);
static BOOL WINAPI FakeQueryFullProcessImageNameW(HANDLE p, DWORD flags, LPWSTR out, PDWORD size);
static BOOL WINAPI FakeCloseHandle(HANDLE h);
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <netlistmgr.h>
#include <shellapi.h>
#include <thread>
#include "../src/native_tray.h"
#include "../src/sys_poll.h"
#include "../src/taskbar_start.h"
#include "../src/taskbar_visibility.h"
#include "../src/explorer_taskbar.h"
struct StopPoller {};   // thrown by the fake wait: the end of a poller run
template <class F> bool FakeTryDetach(F&& work) {   // the poller's loop on this thread, until the fake wait ends it
    try { work(); } catch (const StopPoller&) {}
    return true;
}
#define GetAdaptersAddresses FakeGetAdaptersAddresses
#define GetSystemPowerStatus FakeGetSystemPowerStatus
#define GetLocalTime FakeGetLocalTime
#define GetTimeFormatEx FakeGetTimeFormatEx
#define GetDateFormatEx FakeGetDateFormatEx
#define CoCreateInstance FakeCoCreateInstance
#define CoInitializeEx FakeCoInitializeEx
#define NotifyIpInterfaceChange FakeNotifyIpInterfaceChange
#define NotifyUnicastIpAddressChange FakeNotifyUnicastIpAddressChange
#define WaitForSingleObject FakeWaitForSingleObject
#define TryDetach FakeTryDetach
#define SendInput FakeSendInput
#define AllowSetForegroundWindow FakeAllowSetForegroundWindow
#define GetWindowThreadProcessId FakeGetWindowThreadProcessId
#define OpenProcess FakeOpenProcess
#define QueryFullProcessImageNameW FakeQueryFullProcessImageNameW
#define CloseHandle FakeCloseHandle
#include "../src/system.cpp"
#include "../src/shell_front.cpp"   // the shell input helpers, under the same fakes
#undef SendInput
#undef AllowSetForegroundWindow
#undef GetWindowThreadProcessId
#undef OpenProcess
#undef QueryFullProcessImageNameW
#undef CloseHandle
#undef GetAdaptersAddresses
#undef GetSystemPowerStatus
#undef GetLocalTime
#undef GetTimeFormatEx
#undef GetDateFormatEx
#undef CoCreateInstance
#undef CoInitializeEx
#undef NotifyIpInterfaceChange
#undef NotifyUnicastIpAddressChange
#undef WaitForSingleObject
#undef TryDetach
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>

static int g_logs = 0;
static std::wstring g_lastLog;
static std::wstring* g_pollTrace = nullptr;   // a poller run's calls, in order (PollerRun only)
static bool g_fakeNow = false;                // NowSeconds gives g_now (PollerRun only)
static double g_now = 0;
void Log(const wchar_t* fmt, ...) {
    ++g_logs;
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    g_lastLog = buf;
    if (g_pollTrace) *g_pollTrace += std::wstring(L"Log ") + buf + L" | ";
    char utf8[2048] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    printf("  log: %s\n", utf8);
}
double NowSeconds() {
    if (g_fakeNow) return g_now;
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

#include "portable_check.h"
static const bool kFlushEachCheck = (portable_check::FlushEach() = true);

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
    ULONG type2 = 0;                 // a second adapter after it (0: none), up / with a gateway as set
    bool up2 = true, gateway2 = true;
    int calls = 0;
} g_list;

}   // namespace

static ULONG WINAPI FakeGetAdaptersAddresses(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES out, PULONG size) {
    if (g_list.real) return RealGaa(family, flags, reserved, out, size);
    ++g_list.calls;
    if (g_pollTrace) *g_pollTrace += L"adapters | ";
    ULONG need = (flags & GAA_FLAG_SKIP_MULTICAST) ? g_list.needSkip : g_list.needFull;
    if (g_list.calls > 1) need += g_list.growAfterFirst * (g_list.growsEveryCall ? g_list.calls - 1 : 1);
    if (!out || *size < need) { *size = g_list.windowsSize ? std::max(16383ul, 2 * need) : need; return ERROR_BUFFER_OVERFLOW; }
    ZeroMemory(out, need);
    out->Length = sizeof(*out);
    out->IfType = g_list.type;
    out->OperStatus = IfOperStatusUp;
    out->FirstGatewayAddress = reinterpret_cast<PIP_ADAPTER_GATEWAY_ADDRESS_LH>(reinterpret_cast<BYTE*>(out) + sizeof(*out));
    if (g_list.type2 && need >= 2048) {
        auto* second = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(reinterpret_cast<BYTE*>(out) + 1024);
        second->Length = sizeof(*second);
        second->IfType = g_list.type2;
        second->OperStatus = g_list.up2 ? IfOperStatusUp : IfOperStatusDown;
        second->FirstGatewayAddress = g_list.gateway2 ? reinterpret_cast<PIP_ADAPTER_GATEWAY_ADDRESS_LH>(reinterpret_cast<BYTE*>(second) + sizeof(*second)) : nullptr;
        out->Next = second;
    }
    return NO_ERROR;
}

// ---------------------------------------------------------------- the power status
namespace {

struct FakePower {
    std::atomic<bool> real{ true };       // pass the calls to Windows
    SYSTEM_POWER_STATUS answer{};
    std::atomic<bool> holdNext{ false };  // the next call answers, then waits for `release` (a thread held up before its store)
    std::atomic<bool> fail{ false };      // the call fails (FALSE)
    HANDLE answered = CreateEventW(nullptr, TRUE, FALSE, nullptr), release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
} g_power;

}   // namespace

static BOOL WINAPI FakeGetSystemPowerStatus(LPSYSTEM_POWER_STATUS ps) {
    if (g_power.real) return GetSystemPowerStatus(ps);
    if (g_pollTrace) *g_pollTrace += L"power | ";
    if (g_power.fail) return FALSE;
    *ps = g_power.answer;
    if (g_power.holdNext.exchange(false)) {
        SetEvent(g_power.answered);
        WaitForSingleObject(g_power.release, 5000);
    }
    return TRUE;
}

// ---------------------------------------------------------------- the clock (SysInfo::Tick)
namespace {

struct FakeClock {
    bool real = true;                // pass the calls to Windows
    SYSTEMTIME now{};                // what GetLocalTime gives
    bool formatsFail = false;        // GetTimeFormatEx / GetDateFormatEx write nothing and fail
    bool fixedTime = false;          // GetTimeFormatEx gives the same text whatever the time
    int locals = 0;                  // GetLocalTime calls
    std::wstring trace;              // the calls, with what they were asked
} g_clock;

void ClockAt(int year, int month, int day, int dow, int hour, int minute, int second) {
    g_clock.now = {};
    g_clock.now.wYear = (WORD)year; g_clock.now.wMonth = (WORD)month; g_clock.now.wDay = (WORD)day; g_clock.now.wDayOfWeek = (WORD)dow;
    g_clock.now.wHour = (WORD)hour; g_clock.now.wMinute = (WORD)minute; g_clock.now.wSecond = (WORD)second;
}

}   // namespace

static void WINAPI FakeGetLocalTime(LPSYSTEMTIME st) {
    if (g_clock.real) { GetLocalTime(st); return; }
    ++g_clock.locals;
    g_clock.trace += L"GetLocalTime | ";
    *st = g_clock.now;
}
// No locale: "T" hh:mm, and :ss unless TIME_NOSECONDS; what it was asked goes into the trace
static int WINAPI FakeGetTimeFormatEx(LPCWSTR locale, DWORD flags, const SYSTEMTIME* st, LPCWSTR format, LPWSTR out, int size) {
    if (g_clock.real) return GetTimeFormatEx(locale, flags, st, format, out, size);
    wchar_t asked[160];
    swprintf_s(asked, L"GetTimeFormatEx %ls 0x%lX %ls %d | ", locale ? locale : L"user", flags, format ? format : L"null", size);
    g_clock.trace += asked;
    if (g_clock.formatsFail) return 0;
    if (g_clock.fixedTime) return swprintf_s(out, size, L"T--:--") + 1;
    if (flags & TIME_NOSECONDS) return swprintf_s(out, size, L"T%02u:%02u", st->wHour, st->wMinute) + 1;
    return swprintf_s(out, size, L"T%02u:%02u:%02u", st->wHour, st->wMinute, st->wSecond) + 1;
}
static int WINAPI FakeGetDateFormatEx(LPCWSTR locale, DWORD flags, const SYSTEMTIME* st, LPCWSTR format, LPWSTR out, int size, LPCWSTR calendar) {
    if (g_clock.real) return GetDateFormatEx(locale, flags, st, format, out, size, calendar);
    wchar_t asked[160];
    swprintf_s(asked, L"GetDateFormatEx %ls 0x%lX %ls %d %ls | ", locale ? locale : L"user", flags, format ? format : L"null", size,
               calendar ? calendar : L"null");
    g_clock.trace += asked;
    if (g_clock.formatsFail) return 0;
    return swprintf_s(out, size, L"%ls|%u/%u/%u", format ? format : L"", st->wMonth, st->wDay, st->wDayOfWeek) + 1;
}

// ---------------------------------------------------------------- COM objects of the test's own
namespace {

// The network list manager: GetConnectivity answers as set; everything else is not used by ReadNetwork.
struct FakeNlm : INetworkListManager {
    NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
    HRESULT result = S_OK;
    int asked = 0, refs = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetNetworks(NLM_ENUM_NETWORK, IEnumNetworks**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetNetwork(GUID, INetwork**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetNetworkConnections(IEnumNetworkConnections**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetNetworkConnection(GUID, INetworkConnection**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_IsConnectedToInternet(VARIANT_BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_IsConnected(VARIANT_BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetConnectivity(NLM_CONNECTIVITY* c) override {
        ++asked;
        if (g_pollTrace) *g_pollTrace += L"connectivity | ";
        if (SUCCEEDED(result)) *c = connectivity;
        return result;
    }
    HRESULT STDMETHODCALLTYPE SetSimulatedProfileInfo(NLM_SIMULATED_PROFILE_INFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ClearSimulatedProfileInfo() override { return E_NOTIMPL; }
};

// The default device's volume control: level and mute as set, each read can fail; the calls in order.
struct FakeVolume : IAudioEndpointVolume {
    float level = 0.5f;
    BOOL mute = FALSE;
    bool levelFails = false, muteFails = false;
    int refs = 0;
    std::wstring trace;
    // its register can fail; what AddVolume / ToggleMute set (applied when applySets)
    HRESULT registerResult = S_OK;
    bool applySets = false;
    std::wstring sets;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE RegisterControlChangeNotify(IAudioEndpointVolumeCallback* cb) override {
        trace += L"register ";
        if (g_pollTrace) {
            *g_pollTrace += cb == static_cast<IAudioEndpointVolumeCallback*>(&g_audioWatch) ? L"register-volume(watch)" : L"register-volume(other)";
            *g_pollTrace += FAILED(registerResult) ? L" -> fails | " : L" | ";
        }
        return registerResult;
    }
    HRESULT STDMETHODCALLTYPE UnregisterControlChangeNotify(IAudioEndpointVolumeCallback* cb) override {
        trace += cb == static_cast<IAudioEndpointVolumeCallback*>(&g_audioWatch) ? L"unregister(watch) " : L"unregister(other) ";
        if (g_pollTrace) *g_pollTrace += L"unregister-volume | ";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetChannelCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMasterVolumeLevel(float, LPCGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMasterVolumeLevelScalar(float l, LPCGUID ctx) override {
        trace += L"set-level ";
        wchar_t t[64]; swprintf_s(t, L"set-level %.2f%ls ", l, ctx ? L" ctx" : L"");
        sets += t;
        if (!applySets) return E_NOTIMPL;
        level = l;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMasterVolumeLevel(float*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMasterVolumeLevelScalar(float* l) override {
        trace += L"level ";
        if (g_pollTrace) *g_pollTrace += L"level | ";
        if (levelFails) return E_FAIL;
        *l = level;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetChannelVolumeLevel(UINT, float, LPCGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetChannelVolumeLevelScalar(UINT, float, LPCGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetChannelVolumeLevel(UINT, float*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetChannelVolumeLevelScalar(UINT, float*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMute(BOOL m, LPCGUID ctx) override {
        trace += L"set-mute ";
        sets += std::wstring(L"set-mute ") + std::to_wstring(m) + (ctx ? L" ctx " : L" ");
        if (!applySets) return E_NOTIMPL;
        mute = m;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMute(BOOL* m) override {
        trace += L"mute ";
        if (g_pollTrace) *g_pollTrace += L"mute | ";
        if (muteFails) return E_FAIL;
        *m = mute;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVolumeStepInfo(UINT*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE VolumeStepUp(LPCGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE VolumeStepDown(LPCGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVolumeRange(float*, float*, float*) override { return E_NOTIMPL; }
};

// The default playback device and the device enumerator (the poller's AudioControl::Open): the device gives the
// test's volume control; each call goes into the poller run's trace.
struct FakeDevice : IMMDevice {
    FakeVolume* volume = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD, PROPVARIANT*, void** out) override {
        if (g_pollTrace) *g_pollTrace += IsEqualIID(iid, __uuidof(IAudioEndpointVolume)) ? L"activate(volume) | " : L"activate(other) | ";
        volume->AddRef();
        *out = static_cast<IAudioEndpointVolume*>(volume);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetState(DWORD*) override { return E_NOTIMPL; }
};
struct FakeEnumerator : IMMDeviceEnumerator {
    FakeDevice* device = nullptr;
    HRESULT registerResult = S_OK;          // RegisterEndpointNotificationCallback's answer
    std::vector<HRESULT> defaultResults;    // GetDefaultAudioEndpoint's answers, in turn (then S_OK)
    size_t defaultAt = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** out) override {
        const HRESULT hr = defaultAt < defaultResults.size() ? defaultResults[defaultAt++] : S_OK;
        if (g_pollTrace) {
            *g_pollTrace += flow == eRender && role == eMultimedia ? L"default-device(render, multimedia)" : L"default-device(other)";
            *g_pollTrace += hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ? L" -> not found | " : FAILED(hr) ? L" -> fails | " : L" | ";
        }
        if (FAILED(hr)) { *out = nullptr; return hr; }
        *out = device;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR, IMMDevice**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* c) override {
        if (g_pollTrace) {
            *g_pollTrace += c == static_cast<IMMNotificationClient*>(&g_audioWatch) ? L"register-devices(watch)" : L"register-devices(other)";
            *g_pollTrace += FAILED(registerResult) ? L" -> fails | " : L" | ";
        }
        return registerResult;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient*) override {
        if (g_pollTrace) *g_pollTrace += L"unregister-devices | ";
        return S_OK;
    }
};

// A poller run (PollerRun): the objects CoCreateInstance gives, and what the wait between passes does
struct PollRun {
    FakeVolume volume;
    FakeDevice device;
    FakeEnumerator devices;
    FakeNlm networks;
    SysInfo* sys = nullptr;
    int waits = 0;
    std::function<void(int wait, DWORD ms)> onWait;   // moves g_now, reports changes; may throw StopPoller
    bool woken = false;                               // set by onWait: answer WAIT_OBJECT_0 (else WAIT_TIMEOUT)
    // whether the poller's wake event was really set by the time each wait's step is done
    // ("<wait>:woken" / "<wait>:timeout"), and the objects that cannot be made
    std::wstring wakes;
    bool devicesFail = false, networksFail = false;
};
PollRun* g_run = nullptr;

// IAppVisibility of the test's own (LauncherVisible)
struct FakeAppVisibility : IAppVisibilityDecl {
    HRESULT result = S_OK;
    BOOL shown = FALSE;
    int asked = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetAppVisibilityOnMonitor(HMONITOR, int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsLauncherVisible(BOOL* visible) override {
        ++asked;
        if (FAILED(result)) return result;
        *visible = shown;
        return S_OK;
    }
};
FakeAppVisibility* g_appVisibility = nullptr;   // CoCreateInstance(kAppVisibility) gives it while set
int g_appVisibilityMade = 0;
std::wstring g_appVisibilityAsked;

// the shell input calls: the answers, and the calls in order
struct FakeShell {
    std::wstring trace;
    std::vector<BOOL> allow;          // AllowSetForegroundWindow's answers, in turn (then FALSE)
    size_t allowAt = 0;
    long sendAnswer = -1;             // SendInput's answer (-1: as many as it was given)
    std::map<HWND, DWORD> pids;       // GetWindowThreadProcessId (missing: 0)
    std::map<DWORD, std::wstring> images;   // QueryFullProcessImageNameW (missing: fails)
    std::set<DWORD> openFails;        // OpenProcess fails for these
    std::map<HANDLE, DWORD> open;     // the "process" handles given out (events of the test's own)
    DWORD lastSize = 0;               // the buffer QueryFullProcessImageNameW was given
} g_shell;

}   // namespace

static HRESULT WINAPI FakeCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* out) {
    if (g_appVisibility && IsEqualCLSID(clsid, kAppVisibility)) {
        ++g_appVisibilityMade;
        g_appVisibilityAsked = std::wstring(IsEqualIID(iid, kAppVisibilityIid) ? L"iid IAppVisibility" : L"iid other") +
                               (outer ? L", outer" : L", no outer") + (context == CLSCTX_ALL ? L", CLSCTX_ALL" : L", other context");
        *out = static_cast<IAppVisibilityDecl*>(g_appVisibility);
        return S_OK;
    }
    if (!g_run) return CoCreateInstance(clsid, outer, context, iid, out);
    if (IsEqualCLSID(clsid, __uuidof(MMDeviceEnumerator)) && IsEqualIID(iid, __uuidof(IMMDeviceEnumerator))) {
        if (g_run->devicesFail) { *g_pollTrace += L"create(device enumerator) -> fails | "; *out = nullptr; return REGDB_E_CLASSNOTREG; }
        *g_pollTrace += L"create(device enumerator) | ";
        *out = static_cast<IMMDeviceEnumerator*>(&g_run->devices);
        return S_OK;
    }
    if (IsEqualCLSID(clsid, CLSID_NetworkListManager) && IsEqualIID(iid, __uuidof(INetworkListManager))) {
        if (g_run->networksFail) { *g_pollTrace += L"create(network list manager) -> fails | "; *out = nullptr; return REGDB_E_CLASSNOTREG; }
        *g_pollTrace += L"create(network list manager) | ";
        g_run->networks.AddRef();
        *out = static_cast<INetworkListManager*>(&g_run->networks);
        return S_OK;
    }
    *g_pollTrace += L"create(other) | ";
    *out = nullptr;
    return REGDB_E_CLASSNOTREG;
}
static HRESULT WINAPI FakeCoInitializeEx(LPVOID reserved, DWORD flags) {
    if (!g_run) return CoInitializeEx(reserved, flags);
    *g_pollTrace += flags == COINIT_MULTITHREADED ? L"com(multithreaded) | " : L"com(other) | ";
    return S_OK;
}
static DWORD WINAPI FakeNotifyIpInterfaceChange(ADDRESS_FAMILY family, PIPINTERFACE_CHANGE_CALLBACK cb, PVOID context, BOOLEAN initial, HANDLE* out) {
    if (!g_run) return NotifyIpInterfaceChange(family, cb, context, initial, out);
    *g_pollTrace += family == AF_UNSPEC && !initial && cb == OnIpInterfaceChange ? L"notify(interfaces) | " : L"notify(interfaces, other) | ";
    *out = nullptr;
    return NO_ERROR;
}
static DWORD WINAPI FakeNotifyUnicastIpAddressChange(ADDRESS_FAMILY family, PUNICAST_IPADDRESS_CHANGE_CALLBACK cb, PVOID context, BOOLEAN initial, HANDLE* out) {
    if (!g_run) return NotifyUnicastIpAddressChange(family, cb, context, initial, out);
    *g_pollTrace += family == AF_UNSPEC && !initial && cb == OnAddressChange ? L"notify(addresses) | " : L"notify(addresses, other) | ";
    *out = nullptr;
    return NO_ERROR;
}
// The wait between passes: what is shown when it starts, then the test's step (the clock moved, changes reported)
static DWORD WINAPI FakeWaitForSingleObject(HANDLE h, DWORD ms) {
    if (!g_run || h != g_pollWake) return WaitForSingleObject(h, ms);
    const SysInfo& s = *g_run->sys;
    const battery::State b = s.Battery();
    wchar_t line[160];
    swprintf_s(line, L"wait %lu [v%d volume %.2f%ls net %d battery %d] || ", ms, s.version.load(), s.volume.load(), s.muted ? L" muted" : L"",
               s.net.load(), b.present ? b.percent : -1);
    *g_pollTrace += line;
    g_run->woken = false;
    g_run->onWait(++g_run->waits, ms);   // may throw StopPoller
    const bool set = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;   // the event itself (auto-reset: taken, as the wait would)
    g_run->wakes += std::to_wstring(g_run->waits) + (set ? L":woken " : L":timeout ");
    return g_run->woken ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

// The shell input calls (never the real ones)
static UINT WINAPI FakeSendInput(UINT count, LPINPUT inputs, int size) {
    g_shell.trace += L"SendInput " + std::to_wstring(count) + L" size " + std::to_wstring(size) + L" [";
    for (UINT i = 0; i < count; ++i) {
        const INPUT& in = inputs[i];
        wchar_t e[96];
        swprintf_s(e, L"%ls%ls%02X%ls", i ? L" " : L"", in.type == INPUT_KEYBOARD ? L"" : L"(not a key)", in.ki.wVk,
                   in.ki.dwFlags == KEYEVENTF_KEYUP ? L" up" : in.ki.dwFlags == 0 ? L" down" : L" (other flags)");
        g_shell.trace += e;
        if (in.ki.wScan || in.ki.time || in.ki.dwExtraInfo) g_shell.trace += L"(scan/time/extra set)";
    }
    const UINT answer = g_shell.sendAnswer < 0 ? count : (UINT)g_shell.sendAnswer;
    g_shell.trace += L"] -> " + std::to_wstring(answer) + L" | ";
    return answer;
}
static BOOL WINAPI FakeAllowSetForegroundWindow(DWORD pid) {
    const BOOL r = g_shell.allowAt < g_shell.allow.size() ? g_shell.allow[g_shell.allowAt++] : FALSE;
    g_shell.trace += std::wstring(L"allow ") + (pid == GetCurrentProcessId() ? L"self" : pid == ASFW_ANY ? L"any" : L"other") + (r ? L" -> yes | " : L" -> no | ");
    return r;
}
static DWORD WINAPI FakeGetWindowThreadProcessId(HWND w, LPDWORD pid) {
    const auto it = g_shell.pids.find(w);
    const DWORD p = it == g_shell.pids.end() ? 0 : it->second;
    g_shell.trace += L"pid " + std::to_wstring(p) + L" | ";
    if (pid) *pid = p;
    return p ? 77 : 0;
}
static HANDLE WINAPI FakeOpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    wchar_t t[96];
    swprintf_s(t, L"open %lu 0x%lX%ls", pid, access, inherit ? L" inherit" : L"");
    g_shell.trace += t;
    if (g_shell.openFails.count(pid)) { g_shell.trace += L" -> fails | "; SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    g_shell.trace += L" | ";
    HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_shell.open[h] = pid;
    return h;
}
static BOOL WINAPI FakeQueryFullProcessImageNameW(HANDLE p, DWORD flags, LPWSTR out, PDWORD size) {
    g_shell.lastSize = *size;
    const auto o = g_shell.open.find(p);
    const auto it = o == g_shell.open.end() ? g_shell.images.end() : g_shell.images.find(o->second);
    g_shell.trace += L"image " + std::to_wstring(flags);
    if (it == g_shell.images.end() || it->second.size() >= *size) { g_shell.trace += L" -> fails | "; return FALSE; }
    g_shell.trace += L" | ";
    wmemcpy(out, it->second.c_str(), it->second.size() + 1);
    *size = (DWORD)it->second.size();
    return TRUE;
}
static BOOL WINAPI FakeCloseHandle(HANDLE h) {
    const auto o = g_shell.open.find(h);
    if (o != g_shell.open.end()) { g_shell.trace += L"close " + std::to_wstring(o->second) + L" | "; g_shell.open.erase(o); }
    return CloseHandle(h);
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

// ---------------------------------------------------------------- characterization: what is so now
namespace {

// SysInfo::Tick: the clock text, at most every 0.5 s by the caller's clock; version only when a text changed
void ClockText() {
    const std::wstring calls = L"GetLocalTime | GetTimeFormatEx user 0x2 null 64 | GetDateFormatEx user 0x0 M/d ddd 64 null | ";
    SysInfo sys;
    g_clock = {};
    g_clock.real = false;
    ClockAt(2026, 11, 20, 5, 9, 5, 30);
    sys.Tick(0.25);
    Check(g_clock.locals == 0 && sys.version == 0 && sys.timeText.empty(), "char: Tick 0.25 s after the start (lastFast 0): nothing read yet");
    sys.Tick(0.5);
    Check(g_clock.locals == 1 && sys.version == 1, "char: Tick at 0.5 s: the clock read, the text set, version 1");
    Check(sys.timeText == L"T09:05" && sys.dateText == L"M/d ddd|11/20/5" && sys.timeShort == L"09:05",
          "char: ...the time with the user's format (no seconds), the date as \"M/d ddd\", the short time hh:mm");
    Check(g_clock.trace == calls,
          "char: ...the calls: GetLocalTime, GetTimeFormatEx(user locale, TIME_NOSECONDS, 64), GetDateFormatEx(user locale, 0, \"M/d ddd\", 64)");
    if (g_clock.trace != calls) printf("  trace: %ls\n", g_clock.trace.c_str());
    sys.Tick(0.75);
    sys.Tick(0.96875);
    Check(g_clock.locals == 1, "char: ...0.25 s and 0.47 s later: not read again");
    ClockAt(2026, 11, 20, 5, 9, 5, 31);
    sys.Tick(1.0);
    Check(g_clock.locals == 2 && sys.version == 1, "char: 0.5 s later, the same minute (a second on): read, no version bump");
    ClockAt(2026, 11, 20, 5, 9, 6, 0);
    sys.Tick(1.5);
    Check(g_clock.locals == 3 && sys.version == 2 && sys.timeText == L"T09:06" && sys.timeShort == L"09:06", "char: the next minute: version 2, 09:06");
    sys.Tick(1.5);
    sys.Tick(1.0);
    sys.Tick(1.9375);
    Check(g_clock.locals == 3, "char: the same instant again, a clock 0.5 s behind, then 0.44 s on: not read");
    sys.Tick(2.0);
    Check(g_clock.locals == 4 && sys.version == 2, "char: ...0.5 s after the last read: read (nothing changed)");
    ClockAt(2026, 11, 21, 6, 9, 6, 0);
    sys.Tick(2.5);
    Check(sys.version == 3 && sys.timeText == L"T09:06" && sys.dateText == L"M/d ddd|11/21/6", "char: the date alone changes: version 3");
    // the gate goes from the last read, not from the last gate time + 0.5
    sys.Tick(10.0);
    sys.Tick(10.75);
    const int reads = g_clock.locals;
    sys.Tick(11.125);
    Check(reads == 7 && g_clock.locals == reads, "char: read at 10.0 and 10.75; at 11.125 (0.375 s after the last read) not");
    // the short time is set only with a change of the texts
    g_clock.fixedTime = true;
    ClockAt(2026, 11, 21, 6, 23, 59, 0);
    sys.Tick(20.0);
    const std::wstring shortWas = sys.timeShort;
    const int vWas = sys.version;
    ClockAt(2026, 11, 21, 6, 7, 3, 0);
    sys.Tick(21.0);
    Check(sys.timeText == L"T--:--" && sys.timeShort == shortWas && sys.version == vWas,
          "char: a time text that stays the same while the minute changes: the short time not set either, no bump");
    Check(shortWas == L"23:59", "char: ...the short time set with the change before: 23:59");
    g_clock.fixedTime = false;
    sys.Tick(22.0);
    Check(sys.timeShort == L"07:03" && sys.timeText == L"T07:03", "char: the short time keeps two digits: 07:03");
    ClockAt(2026, 11, 22, 0, 0, 0, 0);
    sys.Tick(23.0);
    Check(sys.timeShort == L"00:00", "char: midnight: 00:00");
    // the formats fail: empty texts, a change all the same (the short time set)
    g_clock.formatsFail = true;
    ClockAt(2026, 11, 22, 0, 0, 1, 0);
    const int vBefore = sys.version;
    sys.Tick(24.0);
    Check(sys.timeText.empty() && sys.dateText.empty() && sys.timeShort == L"00:01" && sys.version == vBefore + 1,
          "char: the formats fail: both texts empty - a change (version bump), the short time 00:01");
    sys.Tick(25.0);
    Check(sys.version == vBefore + 1, "char: ...and failing again: no further bump");
    g_clock = {};
}

// SysInfo::Publish: the volume and mute as read or reported
void Volume() {
    SysInfo sys;
    Check(sys.volume == 0.5f && !sys.muted && sys.net == 1 && sys.version == 0, "char: a new SysInfo: volume 0.5, not muted, network 1 (Ethernet), version 0");
    Check(!sys.Publish(0.5f, false) && sys.version == 0, "char: Publish of what is shown: false, no bump");
    Check(!sys.Publish(0.5009f, false) && sys.volume == 0.5f, "char: 0.0009 off: false, the volume kept as it was");
    Check(sys.Publish(0.5011f, false) && sys.volume == 0.5011f && sys.version == 1, "char: 0.0011 off: true, stored as given, version 1");
    Check(sys.Publish(0.5011f, true) && sys.muted && sys.version == 2, "char: the mute alone: true, version 2");
    Check(!sys.Publish(0.5011f, true) && sys.version == 2, "char: ...the same again: false");
    Check(!sys.Publish(0.5020f, true) && sys.volume == 0.5011f, "char: 0.5020 against 0.5011 (0.0009): false");
    Check(sys.Publish(0.5022f, true) && sys.volume == 0.5022f, "char: 0.5022 against the stored 0.5011, not the last asked: true");
    Check(sys.Publish(0.5010f, true) && sys.volume == 0.5010f, "char: down 0.0012: true");
    Check(sys.Publish(0.f, false) && sys.volume == 0.f && !sys.muted, "char: 0, unmuted: true");
    Check(!sys.Publish(0.001f, false) && sys.volume == 0.f, "char: exactly 0.001 off: false (a change is over 0.001)");
    Check(sys.Publish(1.f, false) && sys.Publish(1.5f, false) && sys.volume == 1.5f, "char: not clamped: 1.5 stored as given");
    Check(sys.version == 7, "char: ...seven changes, seven bumps");

    // the volume control's reports (AudioWatch, the audio service's threads)
    AudioWatch w;
    HANDLE wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w.sys = &sys; w.wake = wake;
    AUDIO_VOLUME_NOTIFICATION_DATA d{};
    d.fMasterVolume = 0.25f; d.bMuted = TRUE;
    const int v = sys.version;
    w.OnNotify(&d);
    Check(sys.volume == 0.25f && sys.muted && sys.version == v + 1, "char: a volume report: published (0.25, muted), one bump");
    w.OnNotify(nullptr);
    w.sys = nullptr;
    d.fMasterVolume = 0.75f;
    w.OnNotify(&d);
    w.sys = &sys;
    Check(sys.volume == 0.25f && sys.version == v + 1, "char: ...no data, or no SysInfo yet: nothing");
    Check(WaitForSingleObject(wake, 0) == WAIT_TIMEOUT && !w.device, "char: ...a volume report wakes nothing");
    w.OnDefaultDeviceChanged(eCapture, eMultimedia, nullptr);
    w.OnDefaultDeviceChanged(eRender, eConsole, nullptr);
    w.OnDefaultDeviceChanged(eRender, eCommunications, nullptr);
    w.OnPropertyValueChanged(nullptr, PROPERTYKEY{});
    Check(!w.device && WaitForSingleObject(wake, 0) == WAIT_TIMEOUT, "char: a new default capture / console / communications device, a property: nothing");
    w.OnDefaultDeviceChanged(eRender, eMultimedia, nullptr);
    Check(w.device && WaitForSingleObject(wake, 0) == WAIT_OBJECT_0, "char: a new default playback device (multimedia): the device flag, the poller woken");
    int devicesSeen = 0;
    for (int i = 0; i < 3; ++i) {
        w.device = false; ResetEvent(wake);
        if (i == 0) w.OnDeviceStateChanged(nullptr, 0);
        if (i == 1) w.OnDeviceAdded(nullptr);
        if (i == 2) w.OnDeviceRemoved(nullptr);
        if (w.device && WaitForSingleObject(wake, 0) == WAIT_OBJECT_0) ++devicesSeen;
    }
    Check(devicesSeen == 3, "char: a device's state changed, added, removed: the flag and the wake each");
    w.device = false; w.wake = nullptr;
    w.OnDeviceAdded(nullptr);
    Check(w.device, "char: ...with no wake event: the flag alone");
    void* out = &w;
    bool ifaces = w.QueryInterface(IID_IUnknown, &out) == S_OK && out == static_cast<IMMNotificationClient*>(&w);
    ifaces = ifaces && w.QueryInterface(__uuidof(IAudioEndpointVolumeCallback), &out) == S_OK && out == static_cast<IAudioEndpointVolumeCallback*>(&w);
    ifaces = ifaces && w.QueryInterface(__uuidof(IMMNotificationClient), &out) == S_OK && out == static_cast<IMMNotificationClient*>(&w);
    ifaces = ifaces && w.QueryInterface(IID_IDispatch, &out) == E_NOINTERFACE && out == nullptr && w.QueryInterface(IID_IUnknown, nullptr) == E_POINTER;
    Check(ifaces && w.AddRef() == 2 && w.Release() == 1, "char: AudioWatch's interfaces: IUnknown as the notification client, the volume callback, nothing else; never counted");
    CloseHandle(wake);

    // the poller's read of the open control (AudioControl::Read, Close)
    FakeVolume vol;
    AudioControl ac;
    bool changed = true;
    Check(!ac.Read(sys, changed) && !changed, "char: AudioControl::Read with no control: false, nothing changed");
    ac.volume = &vol;
    vol.level = 0.4f; vol.mute = FALSE;
    const int v2 = sys.version;
    Check(ac.Read(sys, changed) && changed && sys.volume == 0.4f && !sys.muted && sys.version == v2 + 1 && vol.trace == L"level mute ",
          "char: ...read: the level, then the mute, published (a change)");
    vol.trace.clear();
    Check(ac.Read(sys, changed) && !changed && sys.version == v2 + 1, "char: ...the same again: true, not changed");
    vol.levelFails = true; vol.level = 0.9f; vol.trace.clear();
    Check(!ac.Read(sys, changed) && !changed && sys.volume == 0.4f && vol.trace == L"level ", "char: ...the level fails: false, the mute not asked, nothing published");
    vol.levelFails = false; vol.muteFails = true; vol.trace.clear();
    Check(!ac.Read(sys, changed) && !changed && sys.volume == 0.4f && vol.trace == L"level mute ", "char: ...the mute fails: false, nothing published (not even the level)");
    vol.muteFails = false; vol.trace.clear();
    ac.reporting = true;
    ac.Close(g_audioWatch);
    Check(vol.trace == L"unregister(watch) " && !ac.volume && !ac.reporting && vol.refs == 0, "char: Close of a reporting control: unregistered, let go");
    ac.volume = &vol; vol.trace.clear();
    ac.Close(g_audioWatch);
    Check(vol.trace.empty() && !ac.volume && vol.refs == 0, "char: Close of one that did not report: let go, nothing unregistered");
}

// ReadNetwork: the network button's state from the list manager and the adapter list
void NetworkState() {
    FakeNlm nlm;
    auto read = [&](NLM_CONNECTIVITY c, ULONG type, HRESULT hr = S_OK) {
        g_list = {};
        g_list.real = false; g_list.needSkip = 6000; g_list.needFull = 10000; g_list.type = type;
        nlm.connectivity = c; nlm.result = hr;
        ComPtr<INetworkListManager> p = &nlm;
        const int n = ReadNetwork(p);
        g_list.real = true;
        return std::make_pair(n, p != nullptr);
    };
    const ULONG wifi = IF_TYPE_IEEE80211, wired = IF_TYPE_ETHERNET_CSMACD;
    auto r = read(NLM_CONNECTIVITY_DISCONNECTED, wifi);
    Check(r.first == 0 && g_list.calls == 0 && r.second, "char: ReadNetwork, disconnected: 0 (offline), the adapters not read");
    r = read(NLM_CONNECTIVITY(NLM_CONNECTIVITY_IPV4_NOTRAFFIC | NLM_CONNECTIVITY_IPV6_NOTRAFFIC | NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV6_SUBNET), wifi);
    Check(r.first == 0 && g_list.calls == 0, "char: ...no traffic, or a subnet only: 0 (offline)");
    r = read(NLM_CONNECTIVITY_IPV4_INTERNET, wifi);
    Check(r.first == 2 && g_list.calls == 1, "char: ...IPv4 internet, Wi-Fi up: 2 (Wi-Fi), the adapters read once");
    r = read(NLM_CONNECTIVITY_IPV4_INTERNET, wired);
    Check(r.first == 1, "char: ...IPv4 internet over Ethernet: 1");
    const NLM_CONNECTIVITY each[] = { NLM_CONNECTIVITY_IPV6_INTERNET, NLM_CONNECTIVITY_IPV4_LOCALNETWORK, NLM_CONNECTIVITY_IPV6_LOCALNETWORK };
    bool all = true;
    for (NLM_CONNECTIVITY c : each) if (read(c, wifi).first != 2 || read(c, wired).first != 1) all = false;
    Check(all, "char: ...IPv6 internet, an IPv4 or IPv6 local network: connected the same (2 Wi-Fi, 1 Ethernet)");
    const int asked = nlm.asked;
    r = read(NLM_CONNECTIVITY_IPV4_INTERNET, wifi, E_FAIL);
    Check(r.first == 1 && !r.second && nlm.asked == asked + 1 && g_list.calls == 0 && nlm.refs == 0,
          "char: ...GetConnectivity fails: 1 (Ethernet), the manager let go (made again next time), the adapters not read");
}

// The battery: clamps, what counts as a change, a failed read, the log lines, the tickets
void BatteryMore() {
    SysInfo bat;
    bat.SetBattery({ true, 150, false, false, false, 10000000 });
    Check(bat.Battery() == battery::State{ true, 100, false, false, false, (1 << 21) - 2 }, "char: SetBattery 150 %, 10,000,000 s: read back 100 %, 2,097,150 s");
    const int v = bat.version;
    bat.SetBattery({ true, 120, false, false, false, 9000000 });
    Check(bat.version == v, "char: ...120 %, 9,000,000 s: the same once clamped - no change");
    bat.SetBattery({ true, -3, false, false, false, -5 });
    Check(bat.Battery() == battery::State{ true, 0, false, false, false, -1 } && bat.version == v + 1, "char: -3 %, -5 s: 0 %, no estimate (-1), a change");
    bat.SetBattery({ true, 0, false, false, false, 0 });
    Check(bat.version == v + 2 && bat.Battery().secondsLeft == 0, "char: the estimate alone (-1 -> 0 s): a change");
    bat.SetBattery({ true, 1, false, false, false, 0 });
    Check(bat.version == v + 3, "char: the percent alone: a change");
    bat.SetBattery({ true, 1, false, false, true, 0 });
    Check(bat.version == v + 4 && bat.Battery().saver, "char: the saver alone: a change");

    // PollBattery: a failed read, and the log lines
    SysInfo poll;
    SYSTEM_POWER_STATUS none{}, present{};
    none.ACLineStatus = 1; none.BatteryFlag = 128; none.BatteryLifePercent = 255; none.BatteryLifeTime = 0xFFFFFFFF;
    present.ACLineStatus = 0; present.BatteryFlag = 1; present.BatteryLifePercent = 64; present.BatteryLifeTime = 3600;
    g_power.real = false;
    g_logs = 0;
    g_power.answer = none;
    poll.PollBattery();
    Check(g_logs == 0 && poll.version == 0 && !poll.Battery().present, "char: PollBattery, no battery on a first read: nothing logged, no change");
    g_power.answer = present;
    poll.PollBattery();
    Check(g_logs == 1 && g_lastLog == L"battery: present - shown between the system monitor and the buttons" && poll.version == 1,
          "char: ...a battery: 'battery: present - shown between the system monitor and the buttons'");
    present.BatteryLifePercent = 63;
    g_power.answer = present;
    poll.PollBattery();
    Check(g_logs == 1 && poll.version == 2 && poll.Battery().percent == 63, "char: ...its charge changes: a change, no log");
    g_power.fail = true;
    const uint32_t ticketsBefore = poll.batReads;
    poll.PollBattery();
    g_power.fail = false;
    Check(g_logs == 1 && poll.version == 2 && poll.Battery().percent == 63 && poll.batReads == ticketsBefore + 1,
          "char: ...GetSystemPowerStatus fails: nothing stored or logged (its ticket taken all the same)");
    g_power.answer = none;
    poll.PollBattery();
    Check(g_logs == 2 && g_lastLog == L"battery: none" && poll.version == 3 && !poll.Battery().present, "char: ...gone: 'battery: none'");
    g_power.real = true;

    // the tickets: compared as a difference, so they wrap
    SysInfo t;
    t.batReads = 0xFFFFFFFEu;
    const battery::State a{ true, 40, false, false, false, -1 }, b{ true, 41, false, false, false, -1 }, c{ true, 42, false, false, false, -1 };
    t.SetBattery(a);   // ticket 0xFFFFFFFF
    t.SetBattery(b);   // ticket 0
    Check(t.Battery() == b && t.batReads == 0 && (uint32_t)t.bat.load() == 0, "char: the tickets wrap: 0 after 0xFFFFFFFF is newer (stored)");
    battery::State was;
    Check(!t.StoreBattery(c, 0xFFFFFFF0u, &was) && t.Battery() == b, "char: ...0xFFFFFFF0 after 0: older, dropped (false)");
    Check(t.StoreBattery(c, 0, &was) && was == b && t.Battery() == c, "char: ...the same ticket again: stored, the one before given back");
    Check(!t.StoreBattery(a, 0x80000000u, &was) && t.Battery() == c, "char: ...2^31 ahead: taken as older, dropped");
    Check(t.StoreBattery(a, 0x7FFFFFFFu, &was) && was == c && t.Battery() == a, "char: ...2^31 - 1 ahead: newer, stored");
}

// SysInfo::Start's poller, pass by pass (on this thread, the test's clock, the test's objects): what each pass
// reads, in which order, and what is shown after it - volume, mute, network and battery, and the version
void PollerRun() {
    std::wstring trace;
    PollRun run;
    run.device.volume = &run.volume;
    run.devices.device = &run.device;
    run.volume.level = 0.4f; run.volume.mute = FALSE;
    run.networks.connectivity = NLM_CONNECTIVITY_IPV4_INTERNET;
    SysInfo sys;
    run.sys = &sys;
    g_list = {};
    g_list.real = false; g_list.needSkip = 6000; g_list.needFull = 10000; g_list.type = IF_TYPE_IEEE80211;
    SYSTEM_POWER_STATUS power{};
    power.ACLineStatus = 0; power.BatteryFlag = 1; power.BatteryLifePercent = 64; power.BatteryLifeTime = 3600;
    g_power.answer = power;
    g_power.real = false;
    g_audioWatch.device = false;
    g_netChanged = false;
    g_now = 100;
    run.onWait = [&](int wait, DWORD) {
        switch (wait) {
        case 1:   // a network report a quarter second later: the network is gone
            g_now = 100.25; run.networks.connectivity = NLM_CONNECTIVITY_DISCONNECTED; NetChanged(); run.woken = true; break;
        case 2: g_now = 101; break;
        case 3: {   // the volume control reports 0.6, muted (the audio service's thread)
            AUDIO_VOLUME_NOTIFICATION_DATA d{};
            d.fMasterVolume = 0.6f; d.bMuted = TRUE;
            run.volume.level = 0.6f; run.volume.mute = TRUE;
            g_audioWatch.OnNotify(&d);
            g_now = 101.5; run.woken = true;
            break;
        }
        case 4: g_now = 103; run.networks.connectivity = NLM_CONNECTIVITY_IPV4_INTERNET; g_list.type = IF_TYPE_ETHERNET_CSMACD; break;
        case 5: g_now = 105; power.BatteryLifePercent = 63; g_power.answer = power; run.volume.level = 0.65f; break;   // the level unreported
        case 6: g_audioWatch.OnDefaultDeviceChanged(eRender, eMultimedia, nullptr); g_now = 106; run.woken = true; break;
        case 7: g_now = 107; break;
        case 8: g_now = 121; run.volume.level = 0.7f; break;   // a long sleep; the level changed unreported again
        case 9: break;
        default: throw StopPoller{};
        }
    };
    g_logs = 0;
    g_pollTrace = &trace;
    g_fakeNow = true;
    g_run = &run;
    sys.Start();
    g_run = nullptr;
    g_fakeNow = false;
    g_pollTrace = nullptr;
    g_list.real = true;
    g_power.real = true;
    g_audioWatch.sys = nullptr; g_audioWatch.wake = nullptr; g_audioWatch.device = false;
    if (g_pollWake) { CloseHandle(g_pollWake); g_pollWake = nullptr; }
    g_netChanged = false;

    const std::wstring bind = L"default-device(render, multimedia) | activate(volume) | register-volume(watch) | level | mute | ";
    const std::wstring want =
        // pass 1 (100 s): the volume control opened and read, the network, the battery; 5 s to the battery
        L"com(multithreaded) | notify(interfaces) | notify(addresses) | create(device enumerator) | register-devices(watch) | " + bind +
        L"create(network list manager) | connectivity | adapters | power | Log battery: present - shown between the system monitor and the buttons | "
        L"wait 5000 [v3 volume 0.40 net 2 battery 64] || "
        // pass 2 (100.25 s, a network report): nothing read - the network waits 1 s from its last read
        L"wait 750 [v3 volume 0.40 net 2 battery 64] || "
        // pass 3 (101 s): the network: gone (no adapters read); 2 s to the next (a report was followed)
        L"connectivity | wait 2000 [v4 volume 0.40 net 0 battery 64] || "
        // pass 4 (101.5 s, the volume reported meanwhile): nothing read
        L"wait 1500 [v5 volume 0.60 muted net 0 battery 64] || "
        // pass 5 (103 s): the network: Ethernet
        L"connectivity | adapters | wait 2000 [v6 volume 0.60 muted net 1 battery 64] || "
        // pass 6 (105 s): the network (the same), the battery: 63 %
        L"connectivity | adapters | power | wait 2000 [v7 volume 0.60 muted net 1 battery 63] || "
        // pass 7 (106 s, a new default device): the control closed, opened again and read (0.65, unreported before)
        L"unregister-volume | " + bind + L"wait 1000 [v8 volume 0.65 muted net 1 battery 63] || "
        // pass 8 (107 s): the network; 12 s after the report: the next one 30 s on, the volume's check due at 121
        L"connectivity | adapters | wait 2000 [v8 volume 0.65 muted net 1 battery 63] || "
        // pass 9 (121 s): the volume's check (a change it did not report: logged, opened again at once), the network, the battery
        L"level | mute | Log audio: the volume control stopped reporting changes: opened again | connectivity | adapters | power | "
        L"wait 0 [v9 volume 0.70 muted net 1 battery 63] || "
        // pass 10 (121 s): opened again and read (no change); 5 s to the battery
        L"unregister-volume | " + bind + L"wait 5000 [v9 volume 0.70 muted net 1 battery 63] || ";
    Check(trace == want, "char: the poller, pass by pass: what it reads in which order (volume, network, battery), the waits, what is shown after each pass");
    if (trace != want) {
        printf("  trace: %ls\n", trace.c_str());
        printf("  want:  %ls\n", want.c_str());
    }
    Check(run.waits == 10 && sys.version == 9 && g_logs == 2, "char: ...ten waits, nine changes, two log lines");
    printf("  network list manager asked %d times, references left: %d; volume control references left: %d\n", run.networks.asked, run.networks.refs, run.volume.refs);
    Check(run.networks.asked == 6 && run.networks.refs == 0 && run.volume.refs == 0, "char: ...the network list manager made once and asked 6 times; both objects let go when the run ends");
    // the wake event itself, not the test's word for it: set by the network report (NetChanged) and by
    // the new default device (AudioWatch's wake is the poller's event); a volume report sets nothing
    Check(run.wakes == L"1:woken 2:timeout 3:timeout 4:timeout 5:timeout 6:woken 7:timeout 8:timeout 9:timeout ",
          "char: ...the poller's wake event set by the network report (wait 1) and the new default device (wait 6) only, not by a volume report (wait 3)");
    if (run.wakes != L"1:woken 2:timeout 3:timeout 4:timeout 5:timeout 6:woken 7:timeout 8:timeout 9:timeout ") printf("  wakes: %ls\n", run.wakes.c_str());
}

// a poller run of the test's: the objects wired, the run, everything put back
void RunPoller(PollRun& run, SysInfo& sys, std::wstring& trace) {
    run.device.volume = &run.volume;
    run.devices.device = &run.device;
    run.sys = &sys;
    g_audioWatch.device = false;
    g_netChanged = false;
    g_logs = 0;
    g_pollTrace = &trace;
    g_fakeNow = true;
    g_run = &run;
    sys.Start();
    g_run = nullptr;
    g_fakeNow = false;
    g_pollTrace = nullptr;
    g_list.real = true;
    g_power.real = true;
    g_audioWatch.sys = nullptr; g_audioWatch.wake = nullptr; g_audioWatch.device = false;
    if (g_pollWake) { CloseHandle(g_pollWake); g_pollWake = nullptr; }
    g_netChanged = false;
}
void CheckPoll(const std::wstring& got, const std::wstring& want, const char* what) {
    Check(got == want, what);
    if (got != want) {
        printf("  trace: %ls\n", got.c_str());
        printf("  want:  %ls\n", want.c_str());
    }
}
void PollSetup(PollRun& run) {
    run.volume.level = 0.4f; run.volume.mute = FALSE;
    run.networks.connectivity = NLM_CONNECTIVITY_IPV4_INTERNET;
    g_list = {};
    g_list.real = false; g_list.needSkip = 6000; g_list.needFull = 10000; g_list.type = IF_TYPE_IEEE80211;
    SYSTEM_POWER_STATUS power{};
    power.ACLineStatus = 0; power.BatteryFlag = 1; power.BatteryLifePercent = 64; power.BatteryLifeTime = 3600;
    g_power.answer = power;
    g_power.real = false;
    g_now = 100;
}
const std::wstring kPollStart = L"com(multithreaded) | notify(interfaces) | notify(addresses) | ";
const std::wstring kBatteryLog = L"power | Log battery: present - shown between the system monitor and the buttons | ";

// Neither the enumerator nor the control reports: the default device looked up every 15 s, the control read every
// 0.5 s (a change it shows is no missed report), a failed read opens it again at once, a failed open waits 5 s
void PollerUnreported() {
    std::wstring trace;
    PollRun run;
    PollSetup(run);
    run.devices.registerResult = E_FAIL;
    run.volume.registerResult = E_FAIL;
    SysInfo sys;
    run.onWait = [&](int wait, DWORD) {
        switch (wait) {
        case 1: g_now = 100.5; run.volume.level = 0.45f; break;
        case 2: g_now = 101; run.volume.level = 0.5f; break;
        case 3: g_now = 101.5; run.volume.levelFails = true; break;
        case 4: break;
        case 5: g_now = 105.2503; run.volume.levelFails = false; break;   // not a whole millisecond: the wait rounds up
        case 6: g_now = 106.5; break;
        case 7: g_now = 115.5; break;   // 15 s after the last device look
        default: throw StopPoller{};
        }
    };
    RunPoller(run, sys, trace);
    const std::wstring open = L"default-device(render, multimedia) | activate(volume) | register-volume(watch) -> fails | ";
    const std::wstring want =
        // pass 1 (100 s): the enumerator's register fails (logged), the control's too: read every 0.5 s
        kPollStart + L"create(device enumerator) | register-devices(watch) -> fails | "
        L"Log audio: device changes are not reported (the default device is looked up every 15 s) | " + open + L"level | mute | "
        L"create(network list manager) | connectivity | adapters | " + kBatteryLog + L"wait 500 [v3 volume 0.40 net 2 battery 64] || "
        // pass 2 (100.5 s): the first device look (15 s after 0): opened again, read (0.45)
        + open + L"level | mute | wait 500 [v4 volume 0.45 net 2 battery 64] || "
        // pass 3 (101 s): read: 0.50 - a change it did not report, and it does not report: shown, nothing more
        L"level | mute | wait 500 [v5 volume 0.50 net 2 battery 64] || "
        // pass 4 (101.5 s): the read fails (the mute not asked): wait 0
        L"level | wait 0 [v5 volume 0.50 net 2 battery 64] || "
        // pass 5 (101.5 s): opened again, its read fails: not bound - 5 s to the next open, the battery first (105)
        + open + L"level | wait 3500 [v5 volume 0.50 net 2 battery 64] || "
        // pass 6 (105.2503 s): the battery; the open at 106.5: 1249.7 ms, rounded up
        L"power | wait 1250 [v5 volume 0.50 net 2 battery 64] || "
        // pass 7 (106.5 s): opened and read
        + open + L"level | mute | wait 500 [v5 volume 0.50 net 2 battery 64] || "
        // pass 8 (115.5 s): the next device look (15 s after 100.5): opened again; the battery
        + open + L"level | mute | power | wait 500 [v5 volume 0.50 net 2 battery 64] || ";
    CheckPoll(trace, want, "char: the poller, no reports at all: the device looked up every 15 s, the control read every 0.5 s, a failed read or open, the wait rounded up");
    Check(trace.find(L"stopped reporting") == std::wstring::npos && g_logs == 2,
          "char: ...a change the control shows without reporting is no missed report: two log lines (the enumerator, the battery)");
    Check(trace.find(L"wait 1250 ") != std::wstring::npos, "char: ...a wait of 1249.7 ms asked as 1250 (rounded up, not cut)");
    Check(run.wakes == L"1:timeout 2:timeout 3:timeout 4:timeout 5:timeout 6:timeout 7:timeout ", "char: ...nothing sets the wake event");
}

// The default device not there (ERROR_NOT_FOUND: looked for again in 5 s), the service gone (another error: the
// enumerator let go and made again), and a reporting control that misses changes: logged three times at most
void PollerDevices() {
    std::wstring trace;
    PollRun run;
    PollSetup(run);
    run.devices.defaultResults = { HRESULT_FROM_WIN32(ERROR_NOT_FOUND), E_FAIL };
    SysInfo sys;
    run.onWait = [&](int wait, DWORD) {
        switch (wait) {
        case 1: g_now = 105; break;
        case 2: g_now = 110; break;
        case 3: g_now = 125; run.volume.level = 0.41f; break;   // unreported, at the 15 s check
        case 4: break;
        case 5: g_now = 140; run.volume.level = 0.42f; break;
        case 6: break;
        case 7: g_now = 155; run.volume.level = 0.43f; break;
        case 8: break;
        case 9: g_now = 170; run.volume.level = 0.44f; break;
        case 10: break;
        default: throw StopPoller{};
        }
    };
    RunPoller(run, sys, trace);
    const std::wstring bind = L"default-device(render, multimedia) | activate(volume) | register-volume(watch) | level | mute | ";
    const std::wstring missed = L"Log audio: the volume control stopped reporting changes: opened again | ";
    const std::wstring want =
        // pass 1 (100 s): no default device: nothing let go, the next look in 5 s
        kPollStart + L"create(device enumerator) | register-devices(watch) | default-device(render, multimedia) -> not found | "
        L"create(network list manager) | connectivity | adapters | " + kBatteryLog + L"wait 5000 [v2 volume 0.50 net 2 battery 64] || "
        // pass 2 (105 s): another error: the enumerator's register undone, the enumerator let go
        L"default-device(render, multimedia) -> fails | unregister-devices | power | wait 5000 [v2 volume 0.50 net 2 battery 64] || "
        // pass 3 (110 s): the enumerator made again, the control opened and read (reporting: checked in 15 s)
        L"create(device enumerator) | register-devices(watch) | " + bind + L"power | wait 5000 [v3 volume 0.40 net 2 battery 64] || "
        // pass 4 (125 s): the check: a change it did not report - logged (1), opened again at once
        L"level | mute | " + missed + L"power | wait 0 [v4 volume 0.41 net 2 battery 64] || "
        L"unregister-volume | " + bind + L"wait 5000 [v4 volume 0.41 net 2 battery 64] || "
        // pass 6 (140 s): again (2), with the network
        L"level | mute | " + missed + L"connectivity | adapters | power | wait 0 [v5 volume 0.42 net 2 battery 64] || "
        L"unregister-volume | " + bind + L"wait 5000 [v5 volume 0.42 net 2 battery 64] || "
        // pass 8 (155 s): again (3)
        L"level | mute | " + missed + L"power | wait 0 [v6 volume 0.43 net 2 battery 64] || "
        L"unregister-volume | " + bind + L"wait 5000 [v6 volume 0.43 net 2 battery 64] || "
        // pass 10 (170 s): the fourth: not logged, opened again all the same
        L"level | mute | connectivity | adapters | power | wait 0 [v7 volume 0.44 net 2 battery 64] || "
        L"unregister-volume | " + bind + L"wait 5000 [v7 volume 0.44 net 2 battery 64] || ";
    CheckPoll(trace, want, "char: the poller: no default device (looked for in 5 s), the service gone (the enumerator made again), missed reports");
    Check(g_logs == 4 && run.waits == 11, "char: ...four missed reports, three logged (and the battery): four log lines");
}

// AddVolume / ToggleMute (the buttons, the wheel): what is set on the default device's control, what is shown
void VolumeButtons() {
    std::wstring trace;
    PollRun run;
    run.device.volume = &run.volume;
    run.devices.device = &run.device;
    run.volume.applySets = true;
    SysInfo sys;
    g_pollTrace = &trace;
    g_run = &run;
    auto step = [&](float level, BOOL mute, auto&& act) {
        run.volume.level = level; run.volume.mute = mute;
        run.volume.trace.clear(); run.volume.sets.clear(); trace.clear();
        act();
        return run.volume.trace;
    };
    std::wstring t = step(0.4f, FALSE, [&] { sys.AddVolume(0.1f); });
    Check(t == L"level set-level level mute " && run.volume.sets == L"set-level 0.50 " && sys.volume == 0.5f && !sys.muted,
          "char: AddVolume +0.1 at 0.40: the level read, 0.50 set, then the level and the mute read back and shown");
    Check(trace == L"create(device enumerator) | default-device(render, multimedia) | activate(volume) | level | level | mute | " && run.volume.refs == 0,
          "char: ...the enumerator, the default device (render, multimedia), its control - let go after");
    step(0.95f, FALSE, [&] { sys.AddVolume(0.1f); });
    Check(run.volume.sets == L"set-level 1.00 " && sys.volume == 1.f, "char: ...+0.1 at 0.95: 1.00 (clamped)");
    step(0.02f, FALSE, [&] { sys.AddVolume(-0.05f); });
    Check(run.volume.sets == L"set-level 0.00 " && sys.volume == 0.f, "char: ...-0.05 at 0.02: 0.00 (clamped)");
    sys.Publish(0.3f, true);
    t = step(0.3f, TRUE, [&] { sys.AddVolume(0.02f); });
    Check(t == L"level set-level set-mute level mute " && run.volume.sets == L"set-level 0.32 set-mute 0 " && !sys.muted && sys.volume == 0.3f + 0.02f,
          "char: ...muted, +0.02: the level set, then unmuted; shown 0.32 unmuted");
    sys.Publish(0.32f, true);
    t = step(0.32f, TRUE, [&] { sys.AddVolume(-0.02f); });
    Check(t == L"level set-level level mute " && run.volume.sets == L"set-level 0.30 " && sys.muted, "char: ...muted, -0.02: the level set, still muted (no unmute down)");
    // the reads fail: what was asked is shown
    run.volume.levelFails = run.volume.muteFails = true;
    sys.Publish(0.6f, true);
    step(0.6f, TRUE, [&] { sys.AddVolume(0.1f); });
    Check(run.volume.sets == L"set-level 0.10 set-mute 0 " && sys.volume == 0.1f && !sys.muted,
          "char: ...the reads fail, muted, +0.1: 0 + 0.1 set, unmuted; shown what was asked (0.10, unmuted)");
    sys.Publish(0.6f, true);
    step(0.6f, TRUE, [&] { sys.AddVolume(-0.1f); });
    Check(sys.volume == 0.f && sys.muted, "char: ...the reads fail, muted, -0.1: 0 set, still muted shown");
    run.volume.levelFails = run.volume.muteFails = false;

    // ToggleMute
    sys.Publish(0.5f, false);
    t = step(0.5f, FALSE, [&] { sys.ToggleMute(); });
    Check(t == L"mute set-mute level mute " && run.volume.sets == L"set-mute 1 " && sys.muted && sys.volume == 0.5f,
          "char: ToggleMute, not muted: the mute read, set to 1, the level and the mute read back and shown");
    t = step(0.55f, TRUE, [&] { sys.ToggleMute(); });
    Check(run.volume.sets == L"set-mute 0 " && !sys.muted && sys.volume == 0.55f, "char: ...muted: unmuted, the level read shown (0.55)");
    run.volume.levelFails = run.volume.muteFails = true;
    sys.Publish(0.7f, false);
    step(0.2f, FALSE, [&] { sys.ToggleMute(); });
    Check(run.volume.sets == L"set-mute 1 " && sys.muted && sys.volume == 0.7f,
          "char: ...the reads fail: the mute asked (from FALSE: 1) shown, the volume as it was (0.70, not the device's)");
    run.volume.levelFails = run.volume.muteFails = false;

    // no enumerator, no default device: nothing
    run.devicesFail = true;
    const int v = sys.version;
    t = step(0.1f, FALSE, [&] { sys.AddVolume(0.1f); sys.ToggleMute(); });
    Check(t.empty() && sys.version == v && trace == L"create(device enumerator) -> fails | create(device enumerator) -> fails | ",
          "char: ...no enumerator: AddVolume and ToggleMute do nothing");
    run.devicesFail = false;
    run.devices.defaultResults = { E_FAIL };
    t = step(0.1f, FALSE, [&] { sys.AddVolume(0.1f); });
    Check(t.empty() && sys.version == v, "char: ...no default device: nothing");
    g_run = nullptr;
    g_pollTrace = nullptr;
}

// The network list manager cannot be made; the Wi-Fi test with two adapters; the battery saver
void NetworkAndBatteryMore() {
    std::wstring trace;
    PollRun run;
    run.networksFail = true;
    g_pollTrace = &trace;
    g_run = &run;
    ComPtr<INetworkListManager> nlm;
    const int n = ReadNetwork(nlm);
    g_run = nullptr;
    g_pollTrace = nullptr;
    Check(n == 1 && !nlm && trace == L"create(network list manager) -> fails | " && run.networks.asked == 0,
          "char: ReadNetwork, the network list manager cannot be made: 1 (Ethernet), nothing asked (made again next time)");

    auto two = [](ULONG first, ULONG second, bool up2 = true, bool gateway2 = true) {
        g_list = {};
        g_list.real = false; g_list.needSkip = 6000; g_list.needFull = 10000; g_list.type = first;
        g_list.type2 = second; g_list.up2 = up2; g_list.gateway2 = gateway2;
        const bool up = HasWifiUp();
        g_list.real = true;
        return up;
    };
    const ULONG wifi = IF_TYPE_IEEE80211, wired = IF_TYPE_ETHERNET_CSMACD;
    Check(!two(wifi, wired) && !two(wired, wifi), "char: HasWifiUp, Wi-Fi and Ethernet both up with a gateway (either order): not Wi-Fi");
    Check(two(wifi, wired, false) && two(wifi, wired, true, false), "char: ...the Ethernet adapter down, or without a gateway: Wi-Fi");
    Check(two(wifi, IF_TYPE_TUNNEL) && !two(IF_TYPE_TUNNEL, IF_TYPE_PPP), "char: ...Wi-Fi and a tunnel: Wi-Fi; a tunnel and PPP alone: not");

    SysInfo b;
    SYSTEM_POWER_STATUS ps{};
    ps.ACLineStatus = 0; ps.BatteryFlag = 1; ps.BatteryLifePercent = 55; ps.BatteryLifeTime = 5400; ps.SystemStatusFlag = 1;
    g_power.real = false;
    g_power.answer = ps;
    b.PollBattery();
    Check(b.Battery() == battery::State{ true, 55, false, false, true, 5400 }, "char: PollBattery, on battery with the saver on (SystemStatusFlag 1): 55 %, saver, 5400 s left");
    ps.SystemStatusFlag = 2;
    g_power.answer = ps;
    b.PollBattery();
    Check(!b.Battery().saver, "char: ...SystemStatusFlag 2: no saver");
    ps.ACLineStatus = 1; ps.BatteryFlag = 8; ps.SystemStatusFlag = 1;
    g_power.answer = ps;
    b.PollBattery();
    Check(b.Battery() == battery::State{ true, 55, true, true, true, -1 }, "char: ...charging, plugged in, the saver on: no time left shown");
    g_power.real = true;
}

// The shell input helpers (shell_front.cpp): SendKeyCombo, TakeForegroundRight, ForegroundRightText,
// IsShellPopupWindow, LauncherVisible - with the test's answers; nothing is sent, opened or made
void ShellInput() {
    g_shell = {};
    Check(SendKeyCombo({ VK_LWIN, 'R' }) && g_shell.trace == L"SendInput 4 size 40 [5B down 52 down 52 up 5B up] -> 4 | ",
          "char: SendKeyCombo(Win, R): one SendInput of 4 key events - pressed in order, let go in reverse, the size of INPUT; true when all 4 go in");
    g_shell.trace.clear();
    Check(SendKeyCombo({ VK_CONTROL, VK_SHIFT, VK_ESCAPE }) && g_shell.trace == L"SendInput 6 size 40 [11 down 10 down 1B down 1B up 10 up 11 up] -> 6 | ",
          "char: ...Ctrl+Shift+Esc: 6 events, let go Esc, Shift, Ctrl");
    g_shell.sendAnswer = 3;
    Check(!SendKeyCombo({ VK_LWIN, 'R' }), "char: ...3 of 4 inserted: false");
    g_shell.sendAnswer = 0;
    Check(!SendKeyCombo({ VK_LWIN }), "char: ...none inserted: false");
    g_shell.trace.clear();
    Check(SendKeyCombo({}) && g_shell.trace == L"SendInput 0 size 40 [] -> 0 | ", "char: ...no keys: SendInput of 0 events, true (0 of 0)");
    g_shell.sendAnswer = -1;

    // TakeForegroundRight: probe, the unassigned key 0xE8, probe again
    auto take = [](std::vector<BOOL> allow, long sent) {
        g_shell.trace.clear();
        g_shell.allow = std::move(allow); g_shell.allowAt = 0;
        g_shell.sendAnswer = sent;
        const ForegroundRight r = TakeForegroundRight();
        g_shell.sendAnswer = -1;
        return r;
    };
    Check(take({ TRUE }, -1) == ForegroundRight::Had && g_shell.trace == L"allow self -> yes | ", "char: TakeForegroundRight, the right had: Had, no key sent");
    Check(take({ FALSE, TRUE }, -1) == ForegroundRight::Taken &&
          g_shell.trace == L"allow self -> no | SendInput 2 size 40 [E8 down E8 up] -> 2 | allow self -> yes | ",
          "char: ...not had: 0xE8 pressed and let go (one SendInput of 2), then had: Taken");
    Check(take({ FALSE, FALSE }, -1) == ForegroundRight::Refused, "char: ...the key inserted, still not had: Refused");
    Check(take({ FALSE, FALSE }, 1) == ForegroundRight::NotInserted && take({ FALSE, FALSE }, 0) == ForegroundRight::NotInserted,
          "char: ...1 or 0 of the 2 events inserted, not had: NotInserted");
    Check(take({ FALSE, TRUE }, 0) == ForegroundRight::Taken, "char: ...not inserted, but had after: Taken");
    Check(wcscmp(ForegroundRightText(ForegroundRight::Had), L"had it") == 0 &&
          wcscmp(ForegroundRightText(ForegroundRight::Taken), L"taken with an unassigned key") == 0 &&
          wcscmp(ForegroundRightText(ForegroundRight::Refused), L"refused even after an unassigned key") == 0 &&
          wcscmp(ForegroundRightText(ForegroundRight::NotInserted), L"not taken: the key was not inserted") == 0 &&
          wcscmp(ForegroundRightText((ForegroundRight)7), L"?") == 0,
          "char: ForegroundRightText: 'had it', 'taken with an unassigned key', 'refused even after an unassigned key', 'not taken: the key was not inserted', '?'");

    // IsShellPopupWindow: by the window's program, the last window's answer kept
    g_shell = {};
    const wchar_t* sys = L"C:\\Windows\\SystemApps\\";
    struct W { int hwnd; DWORD pid; std::wstring image; };
    const W ws[] = {
        { 0x101, 101, std::wstring(sys) + L"Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\StartMenuExperienceHost.exe" },
        { 0x102, 102, std::wstring(sys) + L"MicrosoftWindows.Client.CBS_cw5n1h2txyewy\\searchhost.EXE" },
        { 0x103, 103, std::wstring(sys) + L"Microsoft.Windows.Search_cw5n1h2txyewy\\SearchApp.exe" },
        { 0x104, 104, std::wstring(sys) + L"ShellExperienceHost_cw5n1h2txyewy\\ShellExperienceHost.exe" },
        { 0x105, 105, std::wstring(sys) + L"MicrosoftWindows.Client.Core_cw5n1h2txyewy\\ShellHost.exe" },
        { 0x106, 106, L"C:\\Windows\\explorer.exe" },
        { 0x107, 107, L"C:\\Tools\\XShellHost.exe" },
        { 0x108, 108, L"ShellHost.exe" },   // no folder
        { 0x109, 109, L"" },                // (open fails)
        { 0x10A, 110, L"" },                // (the image name fails)
    };
    for (const W& w : ws) {
        g_shell.pids[(HWND)(uintptr_t)w.hwnd] = w.pid;
        if (w.pid == 109) g_shell.openFails.insert(109);
        else if (w.pid != 110) g_shell.images[w.pid] = w.image;
    }
    auto popup = [](int hwnd) { g_shell.trace.clear(); return IsShellPopupWindow((HWND)(uintptr_t)hwnd); };
    Check(popup(0x101) && g_shell.trace == L"pid 101 | open 101 0x1000 | image 0 | close 101 | " && g_shell.lastSize == MAX_PATH,
          "char: IsShellPopupWindow, Start's window: true - its process opened (PROCESS_QUERY_LIMITED_INFORMATION), its image name (MAX_PATH), closed");
    Check(popup(0x101) && g_shell.trace.empty(), "char: ...the same window again: the answer kept, nothing asked");
    bool shells = popup(0x102) && popup(0x103) && popup(0x104) && popup(0x105);
    Check(shells, "char: ...searchhost.EXE (any case), SearchApp.exe, ShellExperienceHost.exe, ShellHost.exe: true");
    Check(!popup(0x106) && g_shell.trace == L"pid 106 | open 106 0x1000 | image 0 | close 106 | ", "char: ...explorer.exe: false (closed all the same)");
    Check(!popup(0x107), "char: ...XShellHost.exe: false (the whole name)");
    Check(popup(0x108), "char: ...an image name without a folder, ShellHost.exe: true");
    Check(!popup(0x109) && g_shell.trace == L"pid 109 | open 109 0x1000 -> fails | ", "char: ...the process cannot be opened: false, nothing closed");
    Check(!popup(0x109) && g_shell.trace.empty(), "char: ...asked again: false kept, nothing asked");
    Check(!popup(0x10A) && g_shell.trace == L"pid 110 | open 110 0x1000 | image 0 -> fails | close 110 | ", "char: ...no image name: false, closed");
    Check(!popup(0x1FF) && g_shell.trace == L"pid 0 | ", "char: ...a window of no process (pid 0): false, nothing opened");
    Check(popup(0x101) && g_shell.trace == L"pid 101 | open 101 0x1000 | image 0 | close 101 | ", "char: ...Start's window after another: asked again, true");
    Check(g_shell.open.empty(), "char: ...every process opened is closed");

    // LauncherVisible: the object made once (the first call), then asked each time
    FakeAppVisibility vis;
    g_appVisibility = &vis;
    vis.shown = TRUE;
    const int a = LauncherVisible();
    vis.shown = FALSE;
    const int b = LauncherVisible();
    vis.shown = 2;
    const int c = LauncherVisible();
    vis.result = E_FAIL;
    const int d = LauncherVisible();
    Check(a == 1 && b == 0 && c == 1 && d == -1, "char: LauncherVisible: shown 1, not 0, any nonzero BOOL 1, IsLauncherVisible fails -1");
    Check(g_appVisibilityMade == 1 && vis.asked == 4 && g_appVisibilityAsked == L"iid IAppVisibility, no outer, CLSCTX_ALL",
          "char: ...the object made once (IAppVisibility, CLSCTX_ALL) and kept, asked 4 times");
    g_appVisibility = nullptr;
    g_shell = {};
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

    ClockText();
    Volume();
    NetworkState();
    BatteryMore();
    PollerRun();
    PollerUnreported();
    PollerDevices();
    VolumeButtons();
    NetworkAndBatteryMore();
    ShellInput();

    return Summary("system_state_test");
}
