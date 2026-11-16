#include "sensors.h"
#include "hw_sample.h"
#include "hw_panel.h"
#include "hwinfo.h"
#include "admin_helper.h"
#include <shellapi.h>

SensorClient g_sensors;
extern Settings g_settings;

static std::wstring HelperPath() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\') + 1) + L"LiquidDockSensors.exe";
}

static int RunHidden(const std::wstring& cmd, DWORD timeoutMs) {
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

void SensorClient::Start() {
    // optional: the HWiNFO reading to use when several CPU temperatures are reported (settings.ini)
    std::wstring preferLabel = FromUtf8(g_settings.hwinfoCpuLabel);
    // The first start after a settings file from before the source with the PawnIO switch on: that dock
    // showed HWiNFO's CPU temperature whenever there was one - keep HWiNFO then, and when PawnIO has gone.
    if (g_settings.sourceFromCpuTemp) {
        g_settings.sourceFromCpuTemp = false;
        hwinfo::Reader probe(preferLabel);
        const bool hwinfoCpu = probe.ReadCpu().found, pawnio = PawnIOInstalled();
        g_settings.sensorSource = sensorlogic::MigratedPawnIOSource(pawnio, hwinfoCpu);
        Log(L"sensors: settings from before the source, PawnIO switch on - %s (PawnIO %s, HWiNFO CPU temperature %s)",
            g_settings.sensorSource == sensorlogic::kSourcePawnIO ? L"PawnIO" : L"HWiNFO Gadget",
            pawnio ? L"installed" : L"missing", hwinfoCpu ? L"reported" : L"none");
        g_settings.Save();
    }
    // the first pass goes by the settings too (Tick keeps these up to date)
    readHWiNFO = Items().ReadsHWiNFO();
    readDrives = Items().ssd;
    // SetPanelOpen wakes the sampler with it when a panel opens; without it the sampler sleeps its period out
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake) Log(L"sensors: no wake event (%lu): an opening panel waits up to 2 s for its first sample", GetLastError());
    try {
        sampler = std::thread([this, preferLabel] {
            hwinfo::Reader hwi(preferLabel);
            hw::SensorThread<hwinfo::Reader> t(hwi);   // what is read when: hw_panel.h
            t.Init();
            for (;;) {
                const DWORD period = t.WaitMs(panelOpen.load());
                if (wake) WaitForSingleObject(wake, period);
                else Sleep(period);
                const bool open = panelOpen.load();
                t.Want(readHWiNFO.load(), readDrives.load());
                LDSensorBlock b = {};
                t.SampleCell(open, GetTickCount64(), b);
                {
                    std::lock_guard<std::mutex> lk(mu);
                    localBlk = b;
                }
                localNew = true;
                PanelData p;
                if (t.SamplePanel(open, GetTickCount64(), p)) {
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        localPanel = std::move(p);
                        localPanelNew = true;
                    }
                    localNew = true;
                }
            }
        });
        sampler.detach();
    } catch (...) {   // no thread to be had: the dock runs without its readings
        Log(L"sensors: no thread to sample on: no readings this run");
    }
}

SensorClient::PanelRoom SensorClient::PanelRoomHint() const {
    // before the first panel sample: the drives DriveList would list (the same rules), the installed
    // modules and whether the adapters promise a VRAM block, looked at once; after it, what it held, but
    // never less than that first look - a window built for it then is not rebuilt for fewer rows (Relayout
    // keeps T) when a later count comes out lower
    static const PanelRoom guess = [] {
        PanelRoom r;
        r.drives = hw::CountPanelDrives();
        r.dimms = sensorlogic::InstalledModules(hw::MemoryModules());
        const hw::VideoInfo& video = hw::Video();
        r.vram = sensorlogic::VramExpected(video.nvidia, video.dedicatedGB);
        return r;
    }();
    // the block with its temperature row: HWiNFO's or NVML's (ShownRoom drops the row where neither is read;
    // NVML's kept room for on an NVIDIA adapter from the start: sensorlogic::NvmlVramTempRoom). The cost: on an
    // NVIDIA GPU whose NVML gives none, a horizontal dock 27 DIP thicker with HWiNFO not read (as with it).
    PanelRoom counted = guess;
    if (panel.sampled)
        counted = { std::max(guess.drives, (int)panel.drives.size()), std::max(guess.dimms, (int)panel.dimms.size()),
                    guess.vram || panel.vram.present, false };
    counted.vramTemp = counted.vram;
    return sensorlogic::ShownRoom(counted, Items(), sensorlogic::NvmlVramTempRoom(vramNvmlTempSeen, hw::Video().nvidia));
}

sensorlogic::ShowItems SensorClient::Items() const {
    sensorlogic::ShowItems s;
    s.source = std::clamp(g_settings.sensorSource, 0, 2);
    s.cpuTemp = g_settings.showCpuTemp; s.gpu = g_settings.showGpu; s.vram = g_settings.showVram;
    s.vramTemp = g_settings.showVramTemp; s.ramTemp = g_settings.showRamTemp; s.ssd = g_settings.showSsd;
    return s;
}

void SensorClient::SetPanelOpen(bool wanted, double now) {
    if (linger.Update(wanted, now)) {
        panelOpen = true;
        if (wake) SetEvent(wake);   // a full sample for the panel now, not at the end of the 2 s wait
    } else if (!linger.open) panelOpen = false;
}

bool SensorClient::PawnIOInstalled() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"PawnIO", SERVICE_QUERY_STATUS);
    bool ok = svc != nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

// Must run on the UI thread right after a user click: Windows only shows a UAC prompt in
// front for the foreground app (otherwise it just flashes on the - hidden - taskbar).
bool SensorClient::InstallHelper(HWND owner) {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    std::wstring path = HelperPath();
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = path.c_str();
    sei.lpParameters = L"--install";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) { Log(L"sensor helper install not started (%lu)", GetLastError()); return false; }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

void SensorClient::ShowHWiNFOGuide() {
    // no owner: the dock window is topmost/no-activate and would leave the box hidden behind it
    MessageBoxW(nullptr,
        L"HWiNFO가 읽은 CPU 온도를 그대로 가져옵니다. 드라이버 설치나 관리자 권한이 필요 없습니다.\n\n"
        L"1. HWiNFO 센서 창 아래쪽 톱니(Configure Sensors)를 엽니다.\n"
        L"2. 'HWiNFO Gadget' 탭에서 'Enable reporting to Gadget'을 체크합니다.\n"
        L"3. 목록에서 CPU 온도 항목을 고르고 'Report value in Gadget'을 체크합니다.\n"
        L"    AMD: CPU (Tctl/Tdie)   ·   Intel: CPU Package\n"
        L"    한국어 HWiNFO는 항목 이름이 한국어입니다. [CPU #0] 아래의 온도 항목(예: 코어 온도)이면 됩니다.\n"
        L"    (그 아래의 내장 그래픽 온도 'CPU GT Cores (Graphics)'는 GPU 온도라 쓰지 않습니다.)\n"
        L"4. 확인을 누르면 몇 초 안에 독에 온도가 표시됩니다.\n\n"
        L"RTSS가 쓰는 '공유 메모리 지원'과는 별개 설정이라 둘 다 켜 둬도 됩니다.",
        L"Liquid Dock - HWiNFO에서 CPU 온도 가져오기", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
}

bool SensorClient::EnablePawnIO(HWND owner) {
    if (!PawnIOInstalled()) {
        // no owner: the dock window is topmost/no-activate and would leave the box hidden behind it
        int r = MessageBoxW(nullptr,
            L"CPU 온도는 두 가지 방법으로 표시할 수 있습니다.\n\n"
            L"1) HWiNFO (드라이버 설치 없음)\n"
            L"    HWiNFO를 실행하고 센서 설정의 'HWiNFO Gadget' 탭에서 CPU 온도에 'Report value in Gadget'을\n"
            L"    체크하면 자동으로 표시됩니다. ('온도 읽기'를 HWiNFO Gadget으로 두고 독의 시스템 칸 우클릭 → HWiNFO 안내)\n\n"
            L"2) PawnIO 드라이버\n"
            L"    LibreHardwareMonitor, FanControl 등이 쓰는 무료 오픈소스 하드웨어 접근 드라이버입니다.\n\n"
            L"PawnIO 공식 페이지를 열까요? 설치한 뒤 '온도 읽기'에서 PawnIO를 다시 골라 주세요.",
            L"Liquid Dock - CPU 온도", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
        if (r == IDYES) ShellExecuteW(nullptr, L"open", L"https://pawnio.eu/", nullptr, nullptr, SW_SHOWNORMAL);
        return false;
    }
    // the helper installed only when it is not the dock's own copy byte for byte (a UAC prompt each time it is)
    const bool install = adminhelper::NeedsInstall();
    if (install && !InstallHelper(owner)) return false;   // the UAC prompt declined: the source stays as it was
    g_settings.sensorSource = sensorlogic::kSourcePawnIO;
    g_settings.Save();
    helperRetry = 20.0;
    if (install) {
        lastHelperTry = NowSeconds();   // the install starts it: the first try, the next one after 20 s
        helperTried = true;
    } else {
        lastHelperTry = -1e9;           // installed already: started through its task on the next look
        helperTried = false;
    }
    return true;
}

// Start the elevated helper through its scheduled task (no UAC prompt once installed).
void SensorClient::StartHelper() {
    if (GetFileAttributesW(HelperPath().c_str()) == INVALID_FILE_ATTRIBUTES) return;
    int rc = RunHidden(SystemExe(L"schtasks.exe") + L" /Run /TN " LD_SENSOR_TASK, 8000);
    Log(L"sensor task run rc=%d", rc);
}

bool SensorClient::HelperTaskRegistered() {
    return RunHidden(SystemExe(L"schtasks.exe") + L" /Query /TN " LD_SENSOR_TASK, 8000) == 0;
}

float SensorClient::Temp(Metric m) const {
    if (!live) return NAN;
    switch (m) {
    case CPU: return (d.flags & LD_HAS_CPU_TEMP) ? d.cpuTemp : NAN;
    case GPU: return (d.flags & LD_HAS_GPU) ? d.gpuTemp : NAN;
    case SSD: return (d.flags & LD_HAS_SSD_TEMP) ? d.ssdTemp : NAN;
    default: return NAN;
    }
}

float SensorClient::ShownTemp(Metric m) const {
    const sensorlogic::ShowItems show = Items();
    const bool shown = m == CPU ? show.CpuTempShown() : m == GPU ? show.gpu : m == SSD ? show.ssd : false;
    return shown ? Temp(m) : NAN;
}

int SensorClient::Severity(Metric m) const { return sensorlogic::Severity(ShownTemp(m), Threshold(m)); }

// What the source in force does not read leaves the readings: a pass the sampler took before the source changed
// (HWiNFO's CPU temperature), the helper's CPU temperature merged before it did (it carries no
// LD_CPU_TEMP_HWINFO), the drives' while they are hidden. Every Tick, not only with a new sample: the source
// switched, the cell and the card go by the new one at once, not by the old one's reading for up to 2 s.
// True when anything went.
bool SensorClient::DropUnread(const sensorlogic::ShowItems& show) {
    const uint32_t before = d.flags;
    if (show.ReadsHWiNFO()) {
        if (!(d.flags & LD_CPU_TEMP_HWINFO)) d.flags &= ~LD_HAS_CPU_TEMP;   // HWiNFO's only
    } else if (show.WantsPawnIO()) {
        if (d.flags & LD_CPU_TEMP_HWINFO) d.flags &= ~LD_HAS_CPU_TEMP;      // the helper's only
        d.flags &= ~(LD_CPU_TEMP_HWINFO | LD_HWINFO);
    } else d.flags &= ~(LD_HAS_CPU_TEMP | LD_CPU_TEMP_HWINFO | LD_HWINFO);
    if (!show.ssd) d.flags &= ~LD_HAS_SSD_TEMP;
    return d.flags != before;
}

// The CPU's temperature switched off (or none read): its load. The GPU hidden: no GPU line at all.
sensorlogic::CellKey SensorClient::CellKeyNow() const {
    sensorlogic::CellKey k;
    k.live = live;
    k.cpu = sensorlogic::MakeCellReading(ShownTemp(CPU), live ? d.cpuLoad : NAN, Threshold(CPU));
    k.gpuLine = Items().gpu;
    if (k.gpuLine) k.gpu = sensorlogic::MakeCellReading(ShownTemp(GPU), live && (d.flags & LD_HAS_GPU_LOAD) ? d.gpuLoad : NAN, Threshold(GPU));
    return k;
}

void SensorClient::ReadHelper(double now) {
    if (!Items().WantsPawnIO()) {
        // its readings are used only with the source PawnIO: no looking for its memory 4 times a second
        // meanwhile (for most users it is not there), none held (turned on again: looked for at once)
        if (view) { UnmapViewOfFile(view); view = nullptr; }
        if (map) { CloseHandle(map); map = nullptr; }
        helperLive = false;
        return;
    }
    if (!view) {
        map = OpenFileMappingW(FILE_MAP_READ, FALSE, LD_SENSOR_MAP);
        if (map) view = (const LDSensorBlock*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(LDSensorBlock));
        if (!view && map) { CloseHandle(map); map = nullptr; }
    }
    if (!view) {
        helperLive = false;
        if (now - lastHelperTry > helperRetry) {
            // again after 20 s, then twice as long each time that did not bring it up (its task deleted, say:
            // a schtasks.exe and a dock.log line every 20 s otherwise), at most every 10 min - as adminhelper::Tick
            if (helperTried) helperRetry = std::min(helperRetry * 2.0, 600.0);
            helperTried = true;
            lastHelperTry = now;
            static bool saidNoThread = false;   // said when it starts, not on every try
            const bool started = TryDetach([] { StartHelper(); });
            if (!started && !saidNoThread) Log(L"sensors: no thread to start the helper on (tried again in %.0f s)", helperRetry);
            saidNoThread = !started;
        }
        return;
    }
    helperRetry = 20.0;   // it is there: if it goes away, started again at once
    helperTried = false;
    LDSensorBlock tmp;
    for (int tries = 0; tries < 4; ++tries) {
        uint32_t s0 = view->seq;
        if (s0 & 1) { Sleep(0); continue; }
        MemoryBarrier();
        memcpy(&tmp, (const void*)view, sizeof(tmp));
        MemoryBarrier();
        if (view->seq == s0) {
            // a new sample, and new by the helper's clock too (GetTickCount64): what it published before the
            // dock looked again (CPU temperature off and on, a restart) is not taken for the present
            if (tmp.magic == LD_SENSOR_MAGIC && s0 != lastSeq && GetTickCount64() - tmp.tick < 5000) {
                lastSeq = s0; helperBlk = tmp; helperLive = true; lastHelperSample = now;
            }
            break;
        }
    }
    if (helperLive && now - lastHelperSample > 5.0) {   // helper died
        helperLive = false;
        UnmapViewOfFile(view); view = nullptr;
        CloseHandle(map); map = nullptr;
    }
}

void SensorClient::Tick(double now) {
    const sensorlogic::ShowItems show = Items();
    // the sampler's next pass: HWiNFO read only when it is the source, the drives asked only while shown
    readHWiNFO = show.ReadsHWiNFO();
    readDrives = show.ssd;
    if (now - lastPoll >= 0.25) {
        lastPoll = now;
        // the helper samples only while this event exists (it also serves the windows of elevated programs:
        // the helper is never stopped for the source, only no longer asked for sensors)
        const bool want = show.WantsPawnIO();
        if (want && !wantEvent) wantEvent = CreateEventW(nullptr, TRUE, TRUE, LD_SENSOR_WANT);
        else if (!want && wantEvent) { CloseHandle(wantEvent); wantEvent = nullptr; }
        ReadHelper(now);
    }
    // the source or the drives switched since the last sample: what is no longer read goes now (the cell redrawn
    // when that changes its look)
    if (DropUnread(show)) {
        version++;
        if (const auto key = CellKeyNow(); !(key == cellKey)) { cellKey = key; cellVersion++; }
    }
    // a new local sample is taken on the pass it arrives (only the helper's memory is looked at 4 times a
    // second): the panel's first sample, asked for when it opens, shows on the next frame
    if (!localNew.exchange(false)) return;

    {
        std::lock_guard<std::mutex> lk(mu);
        d = localBlk;
        if (localPanelNew) { panel = localPanel; localPanelNew = false; }
    }
    if (panel.vram.nvmlTemp) vramNvmlTempSeen = true;
    // a pass the sampler took before the source or the drives changed: nothing of what is no longer read
    DropUnread(show);
    if (helperLive && show.WantsPawnIO()) {
        // the source PawnIO: the CPU temperature is the helper's alone (HWiNFO is not read)
        if (helperBlk.flags & LD_HAS_CPU_TEMP) { d.cpuTemp = helperBlk.cpuTemp; d.flags |= LD_HAS_CPU_TEMP; }
        if (show.ssd && !(d.flags & LD_HAS_SSD_TEMP) && (helperBlk.flags & LD_HAS_SSD_TEMP)) { d.ssdTemp = helperBlk.ssdTemp; d.flags |= LD_HAS_SSD_TEMP; }
        wcsncpy_s(d.status, helperBlk.status, _TRUNCATE);
    }
    live = true;
    version++;
    // the cell's look: after the helper's merge, so its CPU temperature counts; the first sample (going
    // live) differs from the key before it
    if (const auto key = CellKeyNow(); !(key == cellKey)) { cellKey = key; cellVersion++; }

    // overheat: alert once, re-arm after cooling 5 °C or 10 minutes; what the settings hide never alerts, but
    // re-arms by what is read (hidden while it cooled down: shown again and hot, it alerts)
    for (int m = 0; m < Count; ++m) {
        const float t = Temp((Metric)m);
        if (std::isnan(t)) continue;
        const float th = Threshold((Metric)m);
        if (t < th - 5) armed[m] = true;
        if (std::isnan(ShownTemp((Metric)m))) continue;
        if (t >= th && (armed[m] || now - lastAlert[m] > 600)) {
            armed[m] = false;
            lastAlert[m] = now;
            alertPending = true;
            Log(L"overheat alert metric %d: %.1f", m, t);
        }
    }
}
