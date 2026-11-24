#include "admin_helper.h"
#include "admin_client.h"
#include <shlobj.h>
#include <mutex>
#include <thread>
#include <vector>

namespace adminhelper {
namespace {

std::wstring KnownDir(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) { s = p; CoTaskMemFree(p); }
    return s;
}

// Where LiquidDockSensors.exe --install puts the helper (and the only place it runs elevated from).
const std::wstring& InstalledPath() {
    static const std::wstring path = KnownDir(FOLDERID_ProgramFiles) + L"\\LiquidDock\\LiquidDockSensors.exe";
    return path;
}

const std::wstring& Pipe() {
    static const std::wstring name = [] {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        return PipeName(session);
    }();
    return name;
}

// The installed copy's version, 0 when there is none (or it has no version: older than the service).
uint64_t InstalledVersion() {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(InstalledPath().c_str(), &dummy);
    if (!size) return 0;
    std::vector<BYTE> buf(size);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(InstalledPath().c_str(), 0, size, buf.data()) ||
        !VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || len < sizeof(VS_FIXEDFILEINFO))
        return 0;
    return PackVersion(fi->dwFileVersionMS, fi->dwFileVersionLS);
}

bool Installed() { return GetFileAttributesW(InstalledPath().c_str()) != INVALID_FILE_ATTRIBUTES; }

// The copy shipped with the dock, next to it (what --install copies from: admin_helper_process.cpp InstallHelper).
std::wstring ShippedPath() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n < MAX_PATH ? n : 0);
    return s.substr(0, s.find_last_of(L'\\') + 1) + L"LiquidDockSensors.exe";
}

// The whole file (false: none, or it could not be read). A shared read: a running helper's image reads too.
bool ReadWhole(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart <= (64ll << 20);   // the helper: about 1 MB
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        for (size_t at = 0; ok && at < out.size(); at += got)
            ok = ReadFile(f, out.data() + at, (DWORD)(out.size() - at), &got, nullptr) && got > 0;
    }
    CloseHandle(f);
    if (!ok) out.clear();
    return ok;
}

const wchar_t* Verb(Command c) {
    return c == Command::Activate ? L"bring up" : c == Command::Minimize ? L"minimize" : L"close";
}

// Requests made while the helper was not up yet: done once it answers (or dropped after a while).
std::mutex g_mu;
struct Pending { Command c; HWND h; std::wstring app; };
std::vector<Pending> g_pending;
bool g_waiting = false;
double g_lastStart = -1e9;
double g_installStarted = -1e9;   // the install runs (UAC answered, files being copied): no second offer
bool g_saidRunning = false;

// The offer on screen (UI thread only): its message box has no owner, so its loop still delivers clicks
// on the dock. A click on an elevated window meanwhile (a double-click's second one, another app) asks
// nothing again: it waits here with the one that asked - yes: done after it once the helper is up, in the
// order clicked; no: dropped with it (the dock's own calls would not reach these windows either).
bool g_asking = false;
std::vector<Pending> g_asked;

// false: no thread to wait for the helper on (the system has none to give) - these requests are dropped, and the
// caller's own calls go instead (they may not reach the window either), none sent twice.
bool Queue(std::vector<Pending> requests, int seconds, bool start) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (Pending& p : requests) g_pending.push_back(std::move(p));
    if (g_waiting) return true;
    g_waiting = true;
    const bool waits = TryDetach([seconds, start] {
        if (start) StartHelper();
        ULONGLONG until = GetTickCount64() + seconds * 1000ull;
        while (!PipeExists(Pipe()) && GetTickCount64() < until) Sleep(150);
        std::vector<Pending> todo;
        {
            std::lock_guard<std::mutex> lk2(g_mu);
            todo.swap(g_pending);
            g_waiting = false;
        }
        for (auto& p : todo) {
            Result r = Call(Pipe(), p.c, (uint64_t)(uintptr_t)p.h, InstalledPath());
            Log(L"admin helper: %ls %ls %ls (%ls)", r == Result::Ok ? L"did" : L"could not", Verb(p.c), p.app.c_str(), ResultName(r));
        }
    });
    if (waits) return true;
    Log(L"admin helper: no thread to wait for it on: %d request(s) dropped%ls", (int)g_pending.size(), start ? L", not started" : L"");
    g_pending.clear();   // all of them this call's: a thread waiting takes every earlier one with g_waiting
    g_waiting = false;
    return false;
}

// The first time an elevated program's window is clicked (or the installed helper is too old): ask.
bool Offer(const std::wstring& app, bool update) {
    std::wstring text =
        L"'" + app + L"'은(는) 관리자 권한으로 실행 중입니다. Windows는 일반 권한 프로그램이 관리자 권한 창을 "
        L"복원·최소화·닫지 못하게 막아서, 독에서 눌러도 반응이 없습니다.\n\n"
        L"독은 FluentFlyout 위젯 때문에 일반 권한으로 실행됩니다. 대신 관리자 권한 창만 맡아 처리하는 "
        L"Liquid Dock 도우미를 " + std::wstring(update ? L"새 버전으로 바꿀까요?" : L"설치할까요?") + L"\n\n"
        L"• 설치·업데이트할 때마다 Windows 확인 창이 한 번 뜹니다. 그 사이에는 묻지 않고 독과 함께 실행됩니다.\n"
        L"• 도우미는 독이 요청한 앱 창의 복원·최소화·닫기만 하고, 독이 꺼지면 30초 뒤 종료됩니다.\n"
        L"• 설정 → 시스템 → '관리자 권한 앱 창 제어'에서 끄고 켤 수 있습니다.";
    // no owner: the dock window is topmost/no-activate and would leave the box hidden behind it
    int r = MessageBoxW(nullptr, text.c_str(), L"Liquid Dock - 관리자 권한 앱 창",
                        MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    if (r != IDYES) {
        g_settings.adminWindows = 2;
        g_settings.Save();
        Log(L"admin helper: declined (settings: adminWindows=2)");
        return false;
    }
    if (!InstallHelper(nullptr)) return false;
    g_settings.adminWindows = 1;
    g_settings.Save();
    Log(L"admin helper: %ls", update ? L"updating the installed helper" : L"installing");
    return true;
}

bool Do(Command c, HWND h, const std::wstring& app) {
    if (g_settings.adminWindows == 2 || !Elevated(h)) return false;
    Result r = Call(Pipe(), c, (uint64_t)(uintptr_t)h, InstalledPath());
    switch (r) {
    case Result::Ok:
        if (!g_saidRunning) { g_saidRunning = true; Log(L"admin helper: answering (%ls runs as administrator)", app.c_str()); }
        return true;
    case Result::NoWindow:
        return true;   // gone meanwhile: nothing to do
    case Result::NotRunning:
        break;
    default:   // refused, or something else on the pipe: the dock's own calls (they may not work either)
        Log(L"admin helper: could not %ls %ls (%ls)", Verb(c), app.c_str(), ResultName(r));
        return false;
    }
    // no helper answers
    if (Installed() && HelperCurrent(InstalledVersion())) {   // installed: start it, then do it
        Log(L"admin helper: starting it to %ls %ls", Verb(c), app.c_str());
        g_lastStart = NowSeconds();
        return Queue({ { c, h, app } }, 6, true);
    }
    if (NowSeconds() - g_installStarted < 30.0) {   // being installed right now
        return Queue({ { c, h, app } }, 30, false);
    }
    if (g_asking) {   // the offer is on screen: kept for its answer (twice in a row, as a double-click: once)
        if (g_asked.back().c != c || g_asked.back().h != h) g_asked.push_back({ c, h, app });
        return true;
    }
    g_asking = true;
    g_asked.assign(1, { c, h, app });
    const bool yes = Offer(app, Installed());   // the first time, or a helper from before mod.10
    g_asking = false;
    std::vector<Pending> asked;
    asked.swap(g_asked);
    if (!yes) return false;
    g_installStarted = NowSeconds();
    return Queue(std::move(asked), 30, false);   // the install starts it
}

}   // namespace

bool Activate(HWND h, const std::wstring& app) { return Do(Command::Activate, h, app); }
bool Minimize(HWND h, const std::wstring& app) { return Do(Command::Minimize, h, app); }
bool Close(HWND h, const std::wstring& app) { return Do(Command::Close, h, app); }

bool ActivateOver(HWND h, const std::wstring& app) {
    if (g_settings.adminWindows == 2) return false;
    const Result r = Call(Pipe(), Command::Activate, (uint64_t)(uintptr_t)h, InstalledPath());
    static int said = -1;   // a line when the answer changes, not one per click
    if ((int)r != said) {
        said = (int)r;
        if (r == Result::Ok) Log(L"admin helper: bringing up %ls over a window that runs as administrator", app.c_str());
        else Log(L"admin helper: not there to bring up %ls over a window that runs as administrator (%ls)", app.c_str(), ResultName(r));
    }
    static bool saidOld = false;
    if (r == Result::Ok && !saidOld && InstalledVersion() < kFirstFrontVersion) {
        saidOld = true;
        Log(L"admin helper: the installed helper is older than 0.2.0-mod.24 and cannot take the front itself - update it "
            L"(LiquidDockSensors.exe --install, as administrator)");
    }
    return r == Result::Ok;
}

void Tick(double now) {
    static double interval = 20.0;   // grows while starting it does not bring it up (its task deleted, say)
    if (g_settings.adminWindows != 1 || now - g_lastStart < interval) return;
    g_lastStart = now;
    if (PipeExists(Pipe())) { interval = 20.0; return; }
    if (!Installed() || !HelperCurrent(InstalledVersion())) return;   // the next click offers it
    interval = std::min(interval * 2.0, 600.0);
    if (!TryDetach([] { StartHelper(); }))   // the next pass tries again
        Log(L"admin helper: no thread to start it on (tried again in %.0f s)", interval);
}

bool NeedsInstall() {
    std::string installed, shipped;
    const bool haveInstalled = ReadWhole(InstalledPath(), installed), haveShipped = ReadWhole(ShippedPath(), shipped);
    if (HelperNeedsInstall(haveInstalled, installed.data(), installed.size(), haveShipped, shipped.data(), shipped.size())) return true;
    // the same bytes, but its task gone (deleted, or a registration that failed after the copy): installed again,
    // which registers it - the only way back short of a new version. Asked only with a copy to install from.
    return haveShipped && !HelperTaskRegistered();
}

void Enable(HWND owner) {
    // the dock's own copy installed (no UAC prompt); with no copy next to the dock to install from, the installed
    // one only when it has the window service (Tick starts no other: on, it would do nothing)
    if (!NeedsInstall() && HelperCurrent(InstalledVersion())) {
        g_settings.adminWindows = 1;
        g_settings.Save();
        g_lastStart = -1e9;   // start it on the next pass
        return;
    }
    if (InstallHelper(owner)) {
        g_settings.adminWindows = 1;
        g_settings.Save();
        Log(L"admin helper: installing (settings)");
    }
}

}   // namespace adminhelper
