// winsock2.h (which pulls in windows.h) must come before common.h here: NOMINMAX first, or windows.h defines
// min/max macros for the whole file (sys_poll.h's std::min would not compile with MSVC)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include "system.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <netlistmgr.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <thread>
#include "native_tray.h"
#include "sys_poll.h"
#include "taskbar_start.h"
#include "taskbar_visibility.h"
#include "explorer_taskbar.h"

SysInfo g_sys;

static ComPtr<IAudioEndpointVolume> DefaultEndpointVolume() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return nullptr;
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) return nullptr;
    ComPtr<IAudioEndpointVolume> vol;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)vol.GetAddressOf()))) return nullptr;
    return vol;
}

static bool HasWifiUp() {
    // The size Windows gives back is for the entries asked for: ask again with the same flags (the list
    // can also grow between two calls, an address coming), a few times at most
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG len = 16 * 1024, r = ERROR_BUFFER_OVERFLOW;
    std::vector<BYTE> buf;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(len);
        r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &len);
    }
    if (r != NO_ERROR) return false;
    bool wifi = false, wired = false;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress) continue;
        if (a->IfType == IF_TYPE_IEEE80211) wifi = true; else if (a->IfType == IF_TYPE_ETHERNET_CSMACD) wired = true;
    }
    return wifi && !wired;
}

// clock text: cheap, on the UI thread
void SysInfo::Tick(double now) {
    if (now - lastFast < 0.5) return;
    lastFast = now;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t t[64] = {}, d[64] = {}, ts[16];
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, t, 64);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"M/d ddd", d, 64, nullptr);
    swprintf_s(ts, L"%02d:%02d", st.wHour, st.wMinute);
    if (timeText != t || dateText != d) { timeText = t; dateText = d; timeShort = ts; version++; }
}

bool SysInfo::Publish(float level, bool mute) {
    if (std::fabs(level - volume) <= 0.001f && mute == muted) return false;
    volume = level; muted = mute; version++;
    return true;
}

// ---------------------------------------------------------------- the background poller
// Volume, mute, network and battery for the buttons, on their own thread: the audio and network services
// can block for seconds (device changes, resume). When it does what: sys_poll.h.
namespace {

// The default playback device's volume control reports volume and mute changes here, and the device
// enumerator a new default device (or a device coming or going). One object for the process: the audio
// service holds it, so it is never counted or freed. Its callbacks come on the service's threads: they
// only store values or set a flag and wake the poller - no calls back into the audio API from there.
class AudioWatch final : public IMMNotificationClient, public IAudioEndpointVolumeCallback {
public:
    SysInfo* sys = nullptr;
    HANDLE wake = nullptr;
    std::atomic<bool> device{ false };

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, __uuidof(IMMNotificationClient))) *out = static_cast<IMMNotificationClient*>(this);
        else if (IsEqualIID(iid, __uuidof(IAudioEndpointVolumeCallback))) *out = static_cast<IAudioEndpointVolumeCallback*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (d && sys) sys->Publish(d->fMasterVolume, d->bMuted != FALSE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eMultimedia) Changed();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void Changed() { device = true; if (wake) SetEvent(wake); }
};

// The open volume control of the default playback device (poller thread only).
struct AudioControl {
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IAudioEndpointVolume> volume;
    bool reporting = false;
    bool devicesReported = false;   // the enumerator reports a new default device (else: looked up every kVolumeCheck)

    bool Open(AudioWatch& watch) {
        Close(watch);
        if (!devices) {
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices)))) return false;
            devicesReported = SUCCEEDED(devices->RegisterEndpointNotificationCallback(&watch));
            if (!devicesReported)
                Log(L"audio: device changes are not reported (the default device is looked up every %.0f s)", syspoll::kVolumeCheck);
        }
        ComPtr<IMMDevice> dev;
        HRESULT hr = devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev);
        if (FAILED(hr)) {
            if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {   // not just "no device": the service went away, start over
                if (devicesReported) devices->UnregisterEndpointNotificationCallback(&watch);
                devices.Reset();
                devicesReported = false;
            }
            return false;
        }
        if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(volume.GetAddressOf())))) return false;
        reporting = SUCCEEDED(volume->RegisterControlChangeNotify(&watch));
        return true;
    }
    void Close(AudioWatch& watch) {
        if (volume && reporting) volume->UnregisterControlChangeNotify(&watch);
        volume.Reset();
        reporting = false;
    }
    // false: the control can no longer be read. changed: the level or mute was not what was shown.
    bool Read(SysInfo& sys, bool& changed) {
        float level = 0; BOOL mute = FALSE;
        changed = false;
        if (!volume || FAILED(volume->GetMasterVolumeLevelScalar(&level)) || FAILED(volume->GetMute(&mute))) return false;
        changed = sys.Publish(level, mute != FALSE);
        return true;
    }
};

AudioWatch g_audioWatch;
std::atomic<bool> g_netChanged{ false };
HANDLE g_pollWake = nullptr;

void NetChanged() {
    g_netChanged = true;
    if (g_pollWake) SetEvent(g_pollWake);
}
// an interface coming up or going down; an address coming or going (DHCP finishing after the link is up)
void CALLBACK OnIpInterfaceChange(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) { NetChanged(); }
void CALLBACK OnAddressChange(PVOID, PMIB_UNICASTIPADDRESS_ROW, MIB_NOTIFICATION_TYPE) { NetChanged(); }

int ReadNetwork(ComPtr<INetworkListManager>& nlm) {
    if (!nlm && FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nlm)))) return 1;
    NLM_CONNECTIVITY c = NLM_CONNECTIVITY_DISCONNECTED;
    if (FAILED(nlm->GetConnectivity(&c))) { nlm.Reset(); return 1; }   // made again next time
    if (!(c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET | NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV6_LOCALNETWORK)))
        return 0;
    return HasWifiUp() ? 2 : 1;
}

}   // namespace

void SysInfo::Start() {
    g_pollWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_audioWatch.sys = this;
    g_audioWatch.wake = g_pollWake;
    const bool polls = TryDetach([this] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HANDLE ipNotify = nullptr, addressNotify = nullptr;
        if (NotifyIpInterfaceChange(AF_UNSPEC, OnIpInterfaceChange, nullptr, FALSE, &ipNotify) != NO_ERROR)
            Log(L"network: interface changes are not reported (looked at every %.0f s)", syspoll::kNetSafety);
        if (NotifyUnicastIpAddressChange(AF_UNSPEC, OnAddressChange, nullptr, FALSE, &addressNotify) != NO_ERROR)
            Log(L"network: address changes are not reported");
        AudioControl audio;
        ComPtr<INetworkListManager> nlm;
        syspoll::State state;
        bool volumeLost = false;
        double lastDeviceLook = 0;
        int missedReports = 0;
        for (;;) {
            const double now = NowSeconds();
            syspoll::Events events;
            events.device = g_audioWatch.device.exchange(false);
            if (audio.devices && !audio.devicesReported && now - lastDeviceLook >= syspoll::kVolumeCheck) {
                events.device = true;   // no reports of a new default device: look it up now and then
                lastDeviceLook = now;
            }
            events.network = g_netChanged.exchange(false);
            events.volumeLost = volumeLost;
            volumeLost = false;
            const syspoll::Pass pass = syspoll::Next(state, events, now);
            bool bound = false, changed = false;
            if (pass.bindAudio) bound = audio.Open(g_audioWatch) && audio.Read(*this, changed);
            if (pass.readVolume) {
                if (!audio.Read(*this, changed)) volumeLost = true;   // its device went away: open the new one
                else if (changed && audio.reporting) {                // a change it did not report: its reports stopped
                    if (++missedReports <= 3) Log(L"audio: the volume control stopped reporting changes: opened again");
                    volumeLost = true;
                }
            }
            if (pass.readNet) {
                int n = ReadNetwork(nlm);
                if (n != net) { net = n; version++; }
            }
            if (pass.readBattery) PollBattery();
            double sleep = syspoll::Done(state, pass, now, bound, audio.reporting);
            if (volumeLost) sleep = 0;
            const DWORD ms = static_cast<DWORD>(std::ceil(sleep * 1000));
            if (g_pollWake) WaitForSingleObject(g_pollWake, ms);
            else Sleep(ms);   // no event to wake on (never seen): the timed passes still run, nothing spins
        }
    });
    if (!polls) Log(L"system: no thread to poll on: the volume and network not read this run, the battery only at power events");
}

// The poller and WM_POWERBROADCAST (UI thread) both read the battery, unordered: a poller read taken just
// before a plug or unplug can be stored after the UI thread's newer one. Each read takes a ticket before
// it asks Windows and is stored only if no read with a later ticket is - nobody waits for anybody.
void SysInfo::PollBattery() {
    const uint32_t ticket = ++batReads;
    SYSTEM_POWER_STATUS ps{};
    if (!GetSystemPowerStatus(&ps)) return;
    battery::Raw raw;
    raw.acLine = ps.ACLineStatus; raw.flag = ps.BatteryFlag; raw.percent = ps.BatteryLifePercent;
    raw.systemStatus = ps.SystemStatusFlag; raw.lifeTime = ps.BatteryLifeTime;
    const battery::State now = battery::FromRaw(raw);
    battery::State was;
    if (StoreBattery(now, ticket, &was) && now.present != was.present)
        Log(L"battery: %ls", now.present ? L"present - shown between the system monitor and the buttons" : L"none");
}

void SysInfo::SetBattery(const battery::State& set) { StoreBattery(set, ++batReads); }

namespace {

// The battery state and the ticket of its read in one word: bits 0-31 the ticket; 32 present, 33 charging,
// 34 plugged in, 35 saver, 36-42 the percent, 43-63 the seconds left + 1 (0: no estimate). No battery: 0
// above the ticket, which reads back as battery::State{}.
uint64_t PackBattery(const battery::State& s, uint32_t ticket) {
    uint64_t v = ticket;
    if (!s.present) return v;
    v |= 1ull << 32 | (s.charging ? 1ull << 33 : 0) | (s.plugged ? 1ull << 34 : 0) | (s.saver ? 1ull << 35 : 0);
    v |= (uint64_t)std::clamp(s.percent, 0, 100) << 36;
    v |= (uint64_t)(std::clamp(s.secondsLeft, -1, (1 << 21) - 2) + 1) << 43;   // FromRaw: under 7 days
    return v;
}

battery::State UnpackBattery(uint64_t v) {
    battery::State s;
    if (!(v >> 32 & 1)) return s;
    s.present = true;
    s.charging = (v >> 33 & 1) != 0; s.plugged = (v >> 34 & 1) != 0; s.saver = (v >> 35 & 1) != 0;
    s.percent = (int)(v >> 36 & 0x7F);
    s.secondsLeft = (int)(v >> 43) - 1;
    return s;
}

}   // namespace

// false: a read with a later ticket is already stored (this one is older, and dropped)
bool SysInfo::StoreBattery(const battery::State& set, uint32_t ticket, battery::State* was) {
    const uint64_t want = PackBattery(set, ticket);
    uint64_t cur = bat.load();
    do {
        if ((int32_t)(ticket - (uint32_t)cur) < 0) return false;   // the tickets wrap: compared as a difference
    } while (!bat.compare_exchange_weak(cur, want));
    if (was) *was = UnpackBattery(cur);
    if (cur >> 32 != want >> 32) version++;   // the state changed, not just the ticket
    return true;
}

battery::State SysInfo::Battery() const { return UnpackBattery(bat.load()); }

// Both show what the device then says (its own steps may round the level): the same values its change
// report brings, so the poller's check of a reporting control does not take them for a missed report.
void SysInfo::AddVolume(float delta) {
    if (auto v = DefaultEndpointVolume()) {
        float lvl = 0; v->GetMasterVolumeLevelScalar(&lvl);
        lvl = clampf(lvl + delta, 0, 1);
        v->SetMasterVolumeLevelScalar(lvl, nullptr);
        const bool unmute = muted && delta > 0;
        if (unmute) v->SetMute(FALSE, nullptr);
        BOOL m = muted && !unmute;               // what was asked, should a read below fail
        v->GetMasterVolumeLevelScalar(&lvl);
        v->GetMute(&m);
        Publish(lvl, m != FALSE);
    }
}

void SysInfo::ToggleMute() {
    if (auto v = DefaultEndpointVolume()) {
        BOOL m = FALSE; v->GetMute(&m);
        BOOL now = !m;                           // what was asked, should a read below fail
        v->SetMute(now, nullptr);
        float lvl = volume;
        v->GetMasterVolumeLevelScalar(&lvl);
        v->GetMute(&now);
        Publish(lvl, now != FALSE);
    }
}

ForegroundRight TakeForegroundRight() {
    const DWORD me = GetCurrentProcessId();
    if (AllowSetForegroundWindow(me)) return ForegroundRight::Had;
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;   // "unassigned" in the virtual-key table: no program acts on it
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const bool inserted = SendInput(2, in, sizeof(INPUT)) == 2;
    if (AllowSetForegroundWindow(me)) return ForegroundRight::Taken;
    return inserted ? ForegroundRight::Refused : ForegroundRight::NotInserted;
}

const wchar_t* ForegroundRightText(ForegroundRight r) {
    switch (r) {
    case ForegroundRight::Had: return L"had it";
    case ForegroundRight::Taken: return L"taken with an unassigned key";
    case ForegroundRight::Refused: return L"refused even after an unassigned key";
    case ForegroundRight::NotInserted: return L"not taken: the key was not inserted";
    }
    return L"?";
}

bool SendKeyCombo(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    in.reserve(keys.size() * 2);
    for (WORD k : keys) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.end(); it != keys.begin();) { --it; INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i); }
    return SendInput((UINT)in.size(), in.data(), sizeof(INPUT)) == in.size();
}

bool IsShellPopupWindow(HWND w) {
    static HWND last = nullptr; static bool lastResult = false;   // the foreground window rarely changes
    if (w == last) return lastResult;
    last = w; lastResult = false;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!p) return false;
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
        for (const wchar_t* shell : { L"StartMenuExperienceHost.exe", L"SearchHost.exe", L"SearchApp.exe",
                                      L"ShellExperienceHost.exe", L"ShellHost.exe" })
            if (!_wcsicmp(name, shell)) { lastResult = true; break; }
    }
    CloseHandle(p);
    return lastResult;
}
