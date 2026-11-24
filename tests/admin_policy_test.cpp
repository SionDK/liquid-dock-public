// The dock's policy for the windows of programs that run as administrator (src/admin_helper.cpp): when it
// asks the helper, when it offers to install it, the requests it keeps while the helper is starting, how
// often it starts the helper (Tick) and what turning the option on does (Enable). The production file is
// compiled here with fakes for everything that would reach the real helper or the user: the pipe
// (Call, PipeExists), which windows are elevated, %ProgramFiles% (a folder of this test), the offer's
// message box, the install and the start of the helper, the installed file's version, the clock, settings
// and log. The dock's own copy of the helper (Enable compares it with the installed one) is a file this test
// writes next to its exe and removes again. No window but one message-only window of its own (the dock's, for the click that arrives while
// the offer is on screen).
//   cl /std:c++20 /EHsc /utf-8 tests\admin_policy_test.cpp /link user32.lib advapi32.lib ole32.lib
#include "../src/admin_helper.h"
#include "../src/admin_client.h"
#include "../src/sensors.h"
#include <shlobj.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "portable_check.h"

namespace fake {
std::mutex mu;
std::vector<std::wstring> logs;
std::vector<std::pair<adminhelper::Command, HWND>> calls;   // what reached the helper's pipe
std::wstring trustedAsked;                                  // the program the dock trusted on it
std::atomic<int> installs{ 0 }, starts{ 0 }, saves{ 0 };
std::atomic<bool> pipeUp{ false };
adminhelper::Result answer = adminhelper::Result::Ok;      // the helper's answer while its pipe is up
bool installOk = true;
bool taskRegistered = true;                                 // the helper's scheduled task (schtasks /Query)
double now = 1000;
std::wstring programFiles;
std::vector<HWND> elevated;
// the user at the offer: the n-th question asked (from 1), its text -> the answer
std::function<int(int n, const std::wstring& text)> user;
int offers = 0;
std::vector<std::wstring> offerTexts;
}

static adminhelper::Result FakeCall(const std::wstring&, adminhelper::Command c, uint64_t window, const std::wstring& trusted, DWORD = 1500) {
    if (!fake::pipeUp) return adminhelper::Result::NotRunning;
    std::lock_guard<std::mutex> g(fake::mu);
    fake::calls.push_back({ c, (HWND)(uintptr_t)window });
    fake::trustedAsked = trusted;
    return fake::answer;
}
static bool FakePipeExists(const std::wstring&) { return fake::pipeUp; }
static bool FakeElevated(HWND h) {
    for (HWND e : fake::elevated) if (e == h) return true;
    return false;
}
static int WINAPI FakeMessageBox(HWND, LPCWSTR text, LPCWSTR, UINT) {
    int n = ++fake::offers;
    fake::offerTexts.push_back(text);
    return fake::user ? fake::user(n, text) : IDNO;
}
static HRESULT WINAPI FakeKnownFolder(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR* out) {
    size_t bytes = (fake::programFiles.size() + 1) * sizeof(wchar_t);
    *out = (PWSTR)CoTaskMemAlloc(bytes);
    if (!*out) return E_OUTOFMEMORY;
    std::memcpy(*out, fake::programFiles.c_str(), bytes);
    return S_OK;
}
// the installed helper's file version, by path (none: a file without one, as every helper before mod.10)
namespace fake {
std::vector<std::pair<std::wstring, uint64_t>> versions;
VS_FIXEDFILEINFO info{};
uint64_t VersionOf(LPCWSTR path) {
    for (auto& v : versions) if (adminhelper::SameNoCase(v.first, path)) return v.second;
    return 0;
}
void SetVersion(const std::wstring& path, uint64_t version) {
    for (auto& v : versions) if (adminhelper::SameNoCase(v.first, path)) { v.second = version; return; }
    versions.push_back({ path, version });
}
}
static DWORD WINAPI FakeVersionSize(LPCWSTR path, LPDWORD handle) {
    if (handle) *handle = 0;
    return fake::VersionOf(path) ? (DWORD)sizeof(VS_FIXEDFILEINFO) : 0;
}
static BOOL WINAPI FakeVersionInfo(LPCWSTR path, DWORD, DWORD size, LPVOID data) {
    uint64_t v = fake::VersionOf(path);
    if (!v || size < sizeof(v)) return FALSE;
    std::memcpy(data, &v, sizeof(v));   // the "resource" the query below reads
    return TRUE;
}
static BOOL WINAPI FakeVerQuery(LPCVOID data, LPCWSTR, LPVOID* out, PUINT len) {
    uint64_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    fake::info.dwFileVersionMS = (DWORD)(v >> 32);
    fake::info.dwFileVersionLS = (DWORD)v;
    *out = &fake::info;
    *len = sizeof(fake::info);
    return TRUE;
}

// no thread to be had (the system's commit memory used up: std::thread throws), when the test says so
namespace fake { std::atomic<bool> noThread{ false }; }
template <class F> bool FakeTryDetach(F&& work) { return !fake::noThread && TryDetach(std::forward<F>(work)); }

#define TryDetach FakeTryDetach
#define Call FakeCall
#define PipeExists FakePipeExists
#define Elevated FakeElevated
#define MessageBoxW FakeMessageBox
#define SHGetKnownFolderPath FakeKnownFolder
#define GetFileVersionInfoSizeW FakeVersionSize
#define GetFileVersionInfoW FakeVersionInfo
#define VerQueryValueW FakeVerQuery
// The helper's own process - its install (runas: a real UAC prompt) and its scheduled task (schtasks.exe) - is not
// compiled here: the stubs below stand for it (it is tested in sensors_client_test, under that test's fakes). Should
// the policy ever call these itself, this test does not compile (it would run the real ones).
#define ShellExecuteExW DO_NOT_CALL_ShellExecuteExW
#define CreateProcessW DO_NOT_CALL_CreateProcessW
#include "../src/admin_helper.cpp"
#undef ShellExecuteExW
#undef CreateProcessW
#undef TryDetach
#undef Call
#undef PipeExists
#undef Elevated
#undef MessageBoxW
#undef SHGetKnownFolderPath
#undef GetFileVersionInfoSizeW
#undef GetFileVersionInfoW
#undef VerQueryValueW

Settings g_settings;
void Settings::Load() {}
void Settings::Save() const { ++fake::saves; }
double NowSeconds() { return fake::now; }
void Log(const wchar_t* fmt, ...) {
    wchar_t line[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(line, 1024, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> g(fake::mu);
    fake::logs.emplace_back(line);
    std::printf("  log: %ls\n", line);
}
bool adminhelper::InstallHelper(HWND) { ++fake::installs; return fake::installOk; }
void adminhelper::StartHelper() { ++fake::starts; }
bool adminhelper::HelperTaskRegistered() { return fake::taskRegistered; }

namespace {

namespace fs = std::filesystem;
using adminhelper::Command;
using adminhelper::Result;

template <class F> bool WaitFor(F&& ok, DWORD ms = 3000) {
    ULONGLONG until = GetTickCount64() + ms;
    while (!ok()) {
        if (GetTickCount64() > until) return false;
        Sleep(10);
    }
    return true;
}

size_t Calls() { std::lock_guard<std::mutex> g(fake::mu); return fake::calls.size(); }
bool Said(const wchar_t* a) {
    std::lock_guard<std::mutex> g(fake::mu);
    for (const auto& l : fake::logs) if (l.find(a) != std::wstring::npos) return true;
    return false;
}
int SaidCount(const wchar_t* a) {
    std::lock_guard<std::mutex> g(fake::mu);
    int n = 0;
    for (const auto& l : fake::logs) if (l.find(a) != std::wstring::npos) ++n;
    return n;
}
bool Waiting() { std::lock_guard<std::mutex> g(adminhelper::g_mu); return adminhelper::g_waiting; }
// the requests kept while the helper was starting are sent: wait until the worker has sent `n` more
bool Drained(size_t before, size_t n) { return WaitFor([&] { return !Waiting() && Calls() >= before + n; }); }

// The dock's window: a click on it (posted while the offer is up; wParam: the window clicked) reaches the
// dock through the message loop that runs inside the offer's message box, as MessageBoxW's own loop
// delivers it.
constexpr UINT kClick = WM_APP + 1;
HWND g_dock = nullptr;
int g_nestedClicks = 0, g_nestedTaken = 0;   // clicks delivered meanwhile, and how many the helper side took
LRESULT CALLBACK DockProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kClick) {
        ++g_nestedClicks;
        if (adminhelper::Activate((HWND)w, L"Second App")) ++g_nestedTaken;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
void PumpLikeAMessageBox() {
    MSG msg;
    ULONGLONG until = GetTickCount64() + 50;
    while (GetTickCount64() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(1);
    }
}

void Reset() {
    fake::offers = 0;
    fake::offerTexts.clear();
    fake::user = nullptr;
    fake::installs = 0;
    fake::installOk = true;
    fake::pipeUp = false;
    fake::answer = Result::Ok;
    fake::now += 100;   // past every "a moment ago" of the policy
    { std::lock_guard<std::mutex> g(fake::mu); fake::calls.clear(); fake::logs.clear(); }
    g_nestedClicks = g_nestedTaken = 0;
}

}   // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path bin = fs::path(exe).parent_path();
    fake::programFiles = (bin / (L"pf-hunt-admin-" + std::to_wstring(GetCurrentProcessId()))).wstring();
    fs::create_directories(fs::path(fake::programFiles) / L"LiquidDock");
    const fs::path installed = fs::path(fake::programFiles) / L"LiquidDock" / L"LiquidDockSensors.exe";
    Check(adminhelper::InstalledPath() == installed.wstring(), "the installed helper's place: %ProgramFiles%\\LiquidDock (here a folder of the test)");
    std::error_code ec;
    auto install = [&](uint64_t version) {   // a file in its place (its version faked)
        fs::copy_file(exe, installed, fs::copy_options::overwrite_existing, ec);
        fake::SetVersion(installed.wstring(), version);
        return !ec && adminhelper::Installed();
    };
    // a current helper (any version from the window service on), and the one a test PC had installed when
    // the dock was at mod.22 (10-28: %ProgramFiles%\LiquidDock held mod.11)
    const uint64_t mod22 = adminhelper::PackVersion(0x00000002, 0x00000016);   // 0.2.0.22
    const uint64_t mod11 = adminhelper::PackVersion(0x00000002, 0x0000000B);         // 0.2.0.11

    const HWND obs = (HWND)(uintptr_t)0x10010, other = (HWND)(uintptr_t)0x10020, plain = (HWND)(uintptr_t)0x10030;
    fake::elevated = { obs, other };

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DockProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LiquidDockTest.hunt-admin.Dock";
    RegisterClassW(&wc);
    g_dock = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    Check(g_dock != nullptr, "(the dock's message-only window)");

    // ---- an installed helper from an older dock, answering (the first answer of this run): it has the window
    //      service, so it is used as it is. Only a helper older than that is replaced (admin_protocol.h
    //      kFirstServiceVersion): an update of the dock alone asks for no UAC prompt.
    Reset();
    Check(install(mod11) && adminhelper::HelperCurrent(adminhelper::InstalledVersion()), "(mod.11 installed: it has the window service)");
    fake::pipeUp = true;
    g_settings.adminWindows = 1;
    fake::user = [](int, const std::wstring&) { return IDNO; };   // were anything asked
    Check(adminhelper::Activate(obs, L"OBS") && Calls() == 1, "the old helper answers: the window comes up");
    std::printf("  (questions: %d)\n", fake::offers);
    Check(fake::offers == 0 && fake::installs == 0, "and nothing is asked or installed: a helper with the window service stays");
    Check(g_settings.adminWindows == 1, "the option stays on");
    Check(adminhelper::Minimize(obs, L"OBS") && fake::offers == 0, "nor on the next click");
    Check(SaidCount(L"answering") == 1, "the log says once that it answers");
    fs::remove(installed, ec);

    // ---- the option off, or a window of a program at the dock's level: the dock itself, the helper never asked
    Reset();
    fake::pipeUp = true;
    g_settings.adminWindows = 2;
    Check(!adminhelper::Activate(obs, L"OBS") && Calls() == 0, "option off: the dock itself, nothing asked of the helper");
    g_settings.adminWindows = 0;
    Check(!adminhelper::Minimize(plain, L"Notepad") && Calls() == 0, "a window the dock can handle: nothing asked of the helper");

    // ---- the helper answers
    Check(adminhelper::Activate(obs, L"OBS") && Calls() == 1 && fake::calls[0].first == Command::Activate && fake::calls[0].second == obs,
          "an elevated window: the helper brings it up");
    Check(fake::trustedAsked == installed.wstring(), "only the installed helper is trusted on the pipe");
    Check(adminhelper::Minimize(obs, L"OBS") && SaidCount(L"answering") == 0, "and does not say again that it answers");
    fake::answer = Result::NoWindow;
    Check(adminhelper::Close(obs, L"OBS"), "gone meanwhile: nothing more to do");
    fake::answer = Result::WrongClient;
    Check(!adminhelper::Minimize(obs, L"OBS") && Said(L"could not minimize OBS"), "refused: the dock itself, and the log says why");
    Check(fake::offers == 0, "no offer while a helper answers");

    // ---- not installed, the first time: the offer; no -> off for good
    Reset();
    g_settings.adminWindows = 0;
    fake::user = [](int, const std::wstring&) { return IDNO; };
    Check(!adminhelper::Activate(obs, L"OBS") && fake::offers == 1 && g_settings.adminWindows == 2 && fake::installs == 0,
          "declined: the dock itself, adminWindows=2, nothing installed");
    Check(fake::offerTexts.size() == 1 && fake::offerTexts[0].find(L"OBS") != std::wstring::npos && fake::offerTexts[0].find(L"설치할까요") != std::wstring::npos,
          "the offer names the app and asks to install");

    // ---- a second click on the dock while the offer is on screen (its message box runs the dock's message loop)
    Reset();
    g_settings.adminWindows = 0;
    fake::user = [other](int n, const std::wstring&) {
        if (n == 1) {   // the first question is up: the user clicks the second app on the dock
            PostMessageW(g_dock, kClick, (WPARAM)other, 0);
            PumpLikeAMessageBox();
            return IDNO;   // and then answers the box left on screen: no (were there one on top, it got its yes)
        }
        return IDYES;      // a box on top: yes
    };
    adminhelper::Activate(obs, L"OBS");
    std::printf("  (questions: %d, installs started: %d, adminWindows=%d)\n", fake::offers, fake::installs.load(), g_settings.adminWindows);
    Check(g_nestedClicks == 1, "(the click arrived while the question was up)");
    Check(fake::offers == 1, "one question at a time: a click while it is on screen does not ask again");
    Check(!(fake::installs > 0 && g_settings.adminWindows == 2), "and the answers do not leave the helper installed but turned off");
    Check(g_nestedTaken == 1, "the click meanwhile waits for the answer (the dock's own calls are not tried on it)");
    fake::pipeUp = true;   // were a request kept for an install, it would go out now
    Sleep(300);
    Check(!Waiting() && Calls() == 0, "no: neither click goes to the helper");

    // ---- the same, answered yes: the clicks meanwhile (a double-click's second one, then another app) go to
    //      the helper after the first once it is up, in the order clicked, the double-click's twin not again
    Reset();
    g_settings.adminWindows = 0;
    fake::user = [obs, other](int n, const std::wstring&) {
        if (n == 1) {
            PostMessageW(g_dock, kClick, (WPARAM)obs, 0);
            PostMessageW(g_dock, kClick, (WPARAM)other, 0);
            PumpLikeAMessageBox();
        }
        return IDYES;
    };
    Check(adminhelper::Activate(obs, L"OBS") && fake::offers == 1 && fake::installs == 1 && g_settings.adminWindows == 1,
          "yes: one question, one install, adminWindows=1");
    Check(g_nestedClicks == 2 && g_nestedTaken == 2, "both clicks meanwhile kept for the helper");
    Sleep(200);
    Check(Calls() == 0, "(nothing sent while the helper is not up)");
    fake::pipeUp = true;
    Check(Drained(0, 2), "the helper up: the requests sent");
    Sleep(200);
    {
        std::lock_guard<std::mutex> g(fake::mu);
        Check(fake::calls.size() == 2 && fake::calls[0] == std::make_pair(Command::Activate, obs) && fake::calls[1] == std::make_pair(Command::Activate, other),
              "the first app, then the other one: the double-click brings OBS up once");
    }

    // ---- yes: installed, its request kept until the helper answers; a second click meanwhile asks nothing
    Reset();
    g_settings.adminWindows = 0;
    fake::user = [](int, const std::wstring&) { return IDYES; };
    size_t before = Calls();
    Check(adminhelper::Activate(obs, L"OBS") && fake::installs == 1 && g_settings.adminWindows == 1, "accepted: installing, adminWindows=1");
    fake::now += 5;
    Check(adminhelper::Minimize(other, L"Other") && fake::offers == 1 && fake::installs == 1, "while it installs: no second offer, the request kept");
    Sleep(200);
    Check(Calls() == before, "(nothing sent while the helper is not up)");
    fake::pipeUp = true;
    Check(Drained(before, 2), "the helper up: both requests sent");
    {
        std::lock_guard<std::mutex> g(fake::mu);
        Check(fake::calls.size() == 2 && fake::calls[0] == std::make_pair(Command::Activate, obs) && fake::calls[1] == std::make_pair(Command::Minimize, other),
              "in the order they were clicked");
    }
    // the install refused (its UAC prompt declined): the dock itself, asked again next time
    Reset();
    g_settings.adminWindows = 0;
    fake::installOk = false;
    fake::user = [](int, const std::wstring&) { return IDYES; };
    Check(!adminhelper::Activate(obs, L"OBS") && g_settings.adminWindows == 0, "the install not started: the dock itself, the setting as it was");

    // ---- installed from before the window service (no version): the offer is an update
    Reset();
    Check(install(0) && adminhelper::InstalledVersion() == 0, "(an installed helper without a version)");
    g_settings.adminWindows = 1;
    fake::user = [](int, const std::wstring&) { return IDNO; };
    adminhelper::Activate(obs, L"OBS");
    Check(fake::offers == 1 && fake::offerTexts[0].find(L"새 버전으로") != std::wstring::npos, "an old helper: the offer is to update it");

    // ---- installed and current, not running: started once for every click while it starts, requests kept
    Reset();
    Check(install(mod22) &&adminhelper::HelperCurrent(adminhelper::InstalledVersion()), "(a current helper installed)");
    g_settings.adminWindows = 1;
    fake::starts = 0;
    before = Calls();
    bool kept = adminhelper::Activate(obs, L"OBS");
    kept = adminhelper::Minimize(obs, L"OBS") && kept;
    kept = adminhelper::Close(other, L"Other") && kept;
    Check(kept && fake::offers == 0, "not running: the dock waits for it (no offer)");
    Check(WaitFor([] { return fake::starts.load() >= 1; }) && fake::starts == 1, "started once for the three clicks");
    fake::pipeUp = true;
    Check(Drained(before, 3), "up: the three requests sent");
    {
        std::lock_guard<std::mutex> g(fake::mu);
        Check(fake::calls.size() == 3 && fake::calls[0].first == Command::Activate && fake::calls[1].first == Command::Minimize &&
              fake::calls[2] == std::make_pair(Command::Close, other), "in the order they were clicked");
    }
    Check(SaidCount(L"admin helper: did") == 3, "and each one in the log");

    // ---- no thread to wait for it on (apps_model-8): the click goes to the dock's own calls, nothing kept or
    //      sent later, and the next click waits for the helper as before
    Reset();
    g_settings.adminWindows = 1;
    fake::starts = 0;
    fake::noThread = true;
    before = Calls();
    Check(!adminhelper::Activate(obs, L"OBS") && Said(L"no thread to wait for it on: 1 request(s) dropped, not started"),
          "no thread to wait for the helper on: the dock's own calls instead, and the log says why");
    Check(!Waiting() && fake::starts == 0, "...nothing left waiting, the helper not started");
    fake::noThread = false;
    Check(adminhelper::Activate(other, L"Other") && Waiting(), "a thread again: the next click waits for the helper");
    fake::pipeUp = true;
    Check(Drained(before, 1), "up: its request sent");
    Sleep(200);
    {
        std::lock_guard<std::mutex> g(fake::mu);
        Check(fake::calls.size() == 1 && fake::calls[0] == std::make_pair(Command::Activate, other), "only that one: the dropped click is never sent");
    }

    // ---- ActivateOver (mod.24): an ordinary window brought up while a window that runs as administrator is in
    //      front. Only a helper that is on and answering: never started, offered, installed or kept for later -
    //      a "no" sends the dock's own calls at once. One log line when the answer changes, not one per click.
    Reset();
    Check(install(mod22) && adminhelper::HelperCurrent(adminhelper::InstalledVersion()), "(a current helper installed: Activate would start it)");
    fake::pipeUp = true;
    fake::starts = 0;
    fake::saves = 0;
    fake::user = [](int, const std::wstring&) { return IDYES; };   // were anything asked: it would install
    g_settings.adminWindows = 2;
    Check(!adminhelper::ActivateOver(plain, L"Notepad") && Calls() == 0, "over: the option off - false, nothing asked of the helper");
    g_settings.adminWindows = 1;
    Check(adminhelper::ActivateOver(plain, L"Notepad") && Calls() == 1 && fake::calls[0] == std::make_pair(Command::Activate, plain),
          "over: the helper answers - true, one Activate for that window");
    Check(fake::trustedAsked == installed.wstring(), "over: only the installed helper is trusted on the pipe");
    Check(adminhelper::ActivateOver(plain, L"Notepad") && adminhelper::ActivateOver(other, L"Other") && Calls() == 3,
          "over: a request for every click (the window itself need not run as administrator)");
    Check(SaidCount(L"bringing up") == 1 && Said(L"bringing up Notepad over a window that runs as administrator"),
          "over: the log says it once for the three clicks");
    // gone meanwhile: the helper took nothing, so the dock's own calls go (they find no window either)
    fake::answer = Result::NoWindow;
    Check(!adminhelper::ActivateOver(plain, L"Notepad") && Calls() == 4, "over: no such window - false");
    Check(SaidCount(L"not there to bring up") == 1 && Said(L"(no such window)"), "over: the log says the answer changed");
    // not running (installed and current): false at once - not started, offered, installed or queued
    fake::pipeUp = false;
    const double lastStart = adminhelper::g_lastStart;
    bool over = adminhelper::ActivateOver(plain, L"Notepad");
    over = adminhelper::ActivateOver(plain, L"Notepad") || over;
    over = adminhelper::ActivateOver(other, L"Other") || over;
    Check(!over && Calls() == 4, "over: no helper answers - false, nothing sent");
    Sleep(200);   // were a worker started, it would have called StartHelper by now
    Check(fake::starts == 0 && adminhelper::g_lastStart == lastStart, "over: no helper - it is not started");
    Check(fake::offers == 0 && fake::installs == 0, "over: no helper - nothing offered (no message box), nothing installed");
    {
        std::lock_guard<std::mutex> g(adminhelper::g_mu);
        Check(!adminhelper::g_waiting && adminhelper::g_pending.empty(), "over: no helper - nothing kept for later");
    }
    Check(g_settings.adminWindows == 1 && fake::saves == 0, "over: no helper - the setting as it was");
    Check(SaidCount(L"not there to bring up") == 2 && SaidCount(L"(helper not running)") == 1,
          "over: one line for the change to not running, none for the clicks after it");
    // not installed, and while an install runs: still nothing offered or kept
    fs::remove(installed, ec);
    g_settings.adminWindows = 0;
    Check(!adminhelper::ActivateOver(plain, L"Notepad") && fake::offers == 0 && fake::installs == 0 && !Waiting(),
          "over: not installed, never asked - no offer, no install");
    g_settings.adminWindows = 1;
    adminhelper::g_installStarted = fake::now - 5;   // as Activate's "yes" five seconds ago
    Check(!adminhelper::ActivateOver(plain, L"Notepad") && !Waiting() && Calls() == 4, "over: while an install runs - not kept for it");
    adminhelper::g_installStarted = -1e9;
    // up again: one line for that change
    fake::pipeUp = true;
    fake::answer = Result::Ok;
    Check(adminhelper::ActivateOver(plain, L"Notepad") && adminhelper::ActivateOver(plain, L"Notepad") && Calls() == 6,
          "over: the helper back - true again");
    Check(SaidCount(L"bringing up") == 2 && SaidCount(L"not there to bring up") == 2, "over: one more line, for the change back");
    Check(fake::starts == 0 && fake::offers == 0 && fake::saves == 0, "over: in all of it nothing started, offered or saved");
    Check(install(mod22), "(the current helper installed again, for Tick and Enable)");

    // ---- Tick: keeps it running while the option is on, starting it less often while that does not help
    Reset();
    g_settings.adminWindows = 1;
    fake::starts = 0;
    const double t = fake::now;
    auto tickAt = [&](double at) { fake::now = t + at; adminhelper::Tick(fake::now); };
    auto startsAfter = [&](int n) { return WaitFor([&] { return fake::starts.load() >= n; }, 2000) && (Sleep(50), fake::starts.load() == n); };
    tickAt(0);
    Check(startsAfter(1), "not running: started");
    tickAt(39);
    Check(startsAfter(1), "not again within 40 s");
    tickAt(40);
    Check(startsAfter(2), "again after 40 s");
    tickAt(119);
    tickAt(120);
    tickAt(279);
    tickAt(280);
    tickAt(600);
    tickAt(1199);
    tickAt(1200);
    Check(startsAfter(6), "the wait doubles (80, 160, 320 s) up to 10 minutes");
    fake::pipeUp = true;
    tickAt(1800);
    Check(startsAfter(6), "running: not started");
    fake::pipeUp = false;
    tickAt(1820);
    Check(startsAfter(7), "gone again: started after 20 s (the wait starts over)");
    g_settings.adminWindows = 2;
    tickAt(5000);
    Check(startsAfter(7), "the option off: never started");
    g_settings.adminWindows = 1;
    fake::noThread = true;
    tickAt(6000);
    Check(startsAfter(7) && Said(L"no thread to start it on"), "no thread to start it on: not started, the log says so (and the next pass tries again)");
    fake::noThread = false;
    tickAt(7000);
    Check(startsAfter(8), "a thread again: started on the next pass");

    // ---- Enable (the settings switch): installs only when the installed helper is not the dock's own copy, byte for byte
    const fs::path shipped = bin / L"LiquidDockSensors.exe";   // next to the "dock" (this test's exe)
    Check(adminhelper::ShippedPath() == shipped.wstring(), "the shipped copy: LiquidDockSensors.exe next to the dock");
    auto ship = [&](bool sameBytes) {   // the dock's own copy: the installed file's bytes, or one byte other
        std::string bytes;
        if (!adminhelper::ReadWhole(installed.wstring(), bytes) || bytes.empty()) return false;
        if (!sameBytes) bytes.back() ^= 0x5A;
        FILE* f = nullptr;
        if (_wfopen_s(&f, shipped.c_str(), L"wb") != 0 || !f) return false;
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        return std::fclose(f) == 0 && ok;
    };
    Check(ship(true) && !adminhelper::NeedsInstall(), "(the dock's own copy installed: the same bytes)");
    Reset();
    g_settings.adminWindows = 2;
    fake::starts = 0;
    adminhelper::Enable(nullptr);
    Check(g_settings.adminWindows == 1 && fake::installs == 0 && adminhelper::g_lastStart < 0, "on, the same helper installed: on, started on the next pass (no UAC prompt)");
    fake::taskRegistered = false;
    Check(adminhelper::NeedsInstall(), "(the same bytes, its scheduled task gone: installed again)");
    g_settings.adminWindows = 2;
    adminhelper::Enable(nullptr);
    Check(g_settings.adminWindows == 1 && fake::installs == 1, "on, the same helper but its task gone (deleted, a registration that failed): installed again, which registers it");
    fake::taskRegistered = true;
    fake::installs = 0;
    Check(ship(false) && adminhelper::NeedsInstall(), "(a dock with another helper: one byte differs)");
    g_settings.adminWindows = 2;
    adminhelper::Enable(nullptr);
    Check(g_settings.adminWindows == 1 && fake::installs == 1, "on, another helper installed (a rebuild, a newer dock): installed over it (one UAC prompt)");
    fs::remove(shipped, ec);
    Check(!adminhelper::NeedsInstall(), "(no copy next to the dock to install from)");
    g_settings.adminWindows = 2;
    fake::installs = 0;
    adminhelper::Enable(nullptr);
    Check(g_settings.adminWindows == 1 && fake::installs == 0, "on, nothing to install from: the installed helper is used");
    fake::taskRegistered = false;
    Check(!adminhelper::NeedsInstall(), "(nothing to install from, its task gone: nothing to install either)");
    fake::taskRegistered = true;
    {
        // ...but not one from before the window service: Tick would never start it (on, it would do nothing).
        // The install tried as before - with no copy to install from it fails, and the option stays off.
        const uint64_t was = adminhelper::InstalledVersion();
        fake::SetVersion(installed.wstring(), 0);
        g_settings.adminWindows = 2;
        fake::installOk = false;
        adminhelper::Enable(nullptr);
        Check(g_settings.adminWindows == 2 && fake::installs == 1, "on, nothing to install from and the installed helper older than the window service: not on");
        fake::installOk = true;
        fake::installs = 0;
        fake::SetVersion(installed.wstring(), was);
    }
    fs::remove(installed, ec);
    fake::installs = 0;
    g_settings.adminWindows = 2;
    adminhelper::Enable(nullptr);
    Check(g_settings.adminWindows == 1 && fake::installs == 1, "on, not installed: installed (one UAC prompt)");
    g_settings.adminWindows = 2;
    fake::installOk = false;
    adminhelper::Enable(nullptr);
    Check(g_settings.adminWindows == 2, "the install refused: stays off");

    // every worker done before the fakes go away
    WaitFor([] { return !Waiting(); });
    Sleep(200);
    DestroyWindow(g_dock);
    fs::remove_all(fake::programFiles, ec);
    fs::remove(shipped, ec);
    return Summary("admin_policy_test");
}
