#include "common.h"
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <mutex>
#include <variant>

double NowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}

// Settings, pins and logs live in a "data" folder next to the exe (portable; also avoids
// AppData virtualisation when launched from a packaged app).
std::wstring AppDataDir() {
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring s = exe;
        dir = s.substr(0, s.find_last_of(L'\\')) + L"\\data";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

namespace {
// dock.log, opened once per run of the exe. Lines always go to the end of the file (append mode), so a line
// another run adds meanwhile (LogAppend) is not overwritten by this run's next one.
constexpr size_t kEarlyLines = 16;
struct LogFile {
    std::mutex mu;          // Log is called from worker threads too
    FILE* f = nullptr;
    bool append = false;    // false: emptied when opened; while that fails, the next line tries again
    bool session = false;   // LogStartSession came
    bool started = false;   // ...and emptied the file: no more lines are kept for it
    int lines = 0;          // keep the log small however long the dock runs
    std::vector<std::wstring> early;   // this run's first lines with their time: written again at the top of its session
};
LogFile& TheLog() { static LogFile log; return log; }

void OpenLog(LogFile& log) {   // with log.mu held
    const std::wstring path = AppDataDir() + L"\\dock.log";
    if (log.f) { fclose(log.f); log.f = nullptr; }
    if (!log.append) {   // started afresh: its byte order mark, then this run's lines from before (staying elevated)
        FILE* f = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
        if (!f) return;   // held without write sharing: not added to the old log, the next line tries again
        for (const std::wstring& l : log.early) fwprintf(f, L"%s\n", l.c_str());
        fclose(f);
        log.append = true;
        if (log.session) { log.started = true; log.early = {}; }
    }
    log.f = _wfsopen(path.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
}
}

void LogAppend() {
    LogFile& log = TheLog();
    std::lock_guard<std::mutex> lk(log.mu);
    log.append = true;
}

void LogStartSession() {
    LogFile& log = TheLog();
    std::lock_guard<std::mutex> lk(log.mu);
    log.append = false;
    log.session = true;
    log.lines = (int)log.early.size();
    OpenLog(log);
}

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    LogFile& log = TheLog();
    std::lock_guard<std::mutex> lk(log.mu);
    if (++log.lines > 20000) return;
    if (!log.f) OpenLog(log);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t at[16]; swprintf_s(at, L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (log.f) {
        fwprintf(log.f, L"%s %s\n", at, buf);
        fflush(log.f);
    }
    if (!log.started && log.early.size() < kEarlyLines) log.early.push_back(std::wstring(at) + L' ' + buf);
}

std::wstring SystemExe(const wchar_t* name) {
    wchar_t dir[MAX_PATH]; GetSystemDirectoryW(dir, MAX_PATH);
    return L"\"" + std::wstring(dir) + L"\\" + name + L"\"";
}

std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Every setting is one row: key in settings.ini, member, and the accepted range. Unknown keys and
// malformed values are ignored, so older and newer files load with defaults for what is missing.
namespace {
struct Field {
    const char* key;
    std::variant<float Settings::*, int Settings::*, bool Settings::*, Edge Settings::*, std::string Settings::*> member;
    float lo = 0, hi = 0;   // numeric range (ignored for flags and text)
    bool legacy = false;    // read from older files, no longer written
};
const Field kFields[] = {
    { "edge", &Settings::edge, 0, 3 },
    { "iconSize", &Settings::iconSize, 32, 96 },
    { "magnification", &Settings::magnification, 1, 2.1f },   // above the menu's maximum, crowded icons slide away from the pointer
    { "glassStyle", &Settings::glassStyle, 0, 2 },
    { "trayButtons", &Settings::trayButtons },
    { "showDesktopButton", &Settings::showDesktopButton, 0, 0, true },
    { "showThemeButton", &Settings::showThemeButton, 0, 0, true },
    { "showOledButton", &Settings::showOledButton, 0, 0, true },
    { "oledOneMonitor", &Settings::oledOneMonitor },
    { "trayIcons", &Settings::trayIcons },
    { "fluentFlyout", &Settings::fluentFlyout },
    { "clockSize", &Settings::clockSize, 0, 1 },
    { "clockFormat", &Settings::clockFormat, 0, 1 },
    { "sensorSize", &Settings::sensorSize, 0, 1 },
    { "iconSpacing", &Settings::iconSpacing, 0, 2 },
    { "iconGap", &Settings::iconGap, 0, 60 },
    { "dockThickness", &Settings::dockThickness, 75, 150 },
    { "edgeGap", &Settings::edgeGap, 0, 40 },
    { "edgeOffset", &Settings::edgeOffset, -100, 100 },
    { "autoHide", &Settings::autoHide },
    { "secondaryAlwaysShown", &Settings::secondaryAlwaysShown },
    { "showWithStart", &Settings::showWithStart },
    { "showDelay", &Settings::showDelay, 0, 2 },
    { "hideDelay", &Settings::hideDelay, 0, 3 },
    { "autostart", &Settings::autostart },
    { "startMenuExtras", &Settings::startMenuExtras },
    { "shiftRunsAsAdmin", &Settings::shiftRunsAsAdmin },
    { "taskbarAppButtons", &Settings::taskbarAppButtons },
    { "revealHotkey", &Settings::revealHotkey, 0, 3 },
    { "sensorSource", &Settings::sensorSource, 0, 2 },
    { "cpuTemp", &Settings::cpuTemp, 0, 0, true },
    { "showCpuTemp", &Settings::showCpuTemp },
    { "showGpu", &Settings::showGpu },
    { "showVram", &Settings::showVram },
    { "showVramTemp", &Settings::showVramTemp },
    { "showRamTemp", &Settings::showRamTemp },
    { "showSsd", &Settings::showSsd },
    { "powerView", &Settings::powerView, 0, 2 },
    { "showCpuPower", &Settings::showCpuPower },
    { "showGpuPower", &Settings::showGpuPower },
    { "adminWindows", &Settings::adminWindows, 0, 2 },
    { "hwinfoCpuLabel", &Settings::hwinfoCpuLabel },
    { "pinsVersion", &Settings::pinsVersion, 0, 1000000 },
};
}

void Settings::Load() {
    std::ifstream f(AppDataDir() + L"\\settings.ini");
    std::string line;
    bool sawButtons = false, sawDockKey = false, sawSource = false, sawCpuTemp = false, first = true;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);   // saved by an editor as "UTF-8 with BOM"
        first = false;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        for (auto& fd : kFields) {
            if (k != fd.key) continue;
            if (k != "autostart") sawDockKey = true;   // the installer's first file holds only autostart=1
            if (k == "sensorSource") sawSource = true;
            if (k == "cpuTemp") sawCpuTemp = true;
            try {
                std::visit([&](auto m) {
                    using T = std::remove_cvref_t<decltype(this->*m)>;
                    if constexpr (std::is_same_v<T, std::string>) { this->*m = v; if (k == "trayButtons") sawButtons = true; }
                    else if constexpr (std::is_same_v<T, bool>) this->*m = std::stoi(v) != 0;
                    else if constexpr (std::is_same_v<T, float>) {   // "nan" would pass the clamp: ignored like any malformed value
                        const float x = std::stof(v);
                        if (!std::isnan(x)) this->*m = std::clamp(x, fd.lo, fd.hi);
                    }
                    else if constexpr (std::is_same_v<T, int>) this->*m = std::clamp(std::stoi(v), (int)fd.lo, (int)fd.hi);
                    else this->*m = (T)std::clamp(std::stoi(v), (int)fd.lo, (int)fd.hi);
                }, fd.member);
            } catch (...) {}
            break;
        }
    }
    // A file from before the sensor source: its "CPU 온도 직접 읽기 (PawnIO)" switch on is PawnIO; off (or no
    // file, or one from before that switch) is HWiNFO, which that dock always read.
    // That switch on is settled at the sensors' start (SensorClient::Start: PawnIO missing or HWiNFO
    // reporting keeps HWiNFO).
    sourceFromCpuTemp = !sawSource && sawCpuTemp && cpuTemp;
    if (!sawSource) sensorSource = sourceFromCpuTemp ? 2 : 1;
    // A file from before the button list: carry its three on/off choices over. No file, or the installer's
    // first one, is a new install: it keeps the default list.
    if (!sawButtons && sawDockKey) {
        trayButtons.clear();
        for (auto [on, key] : { std::pair{ showDesktopButton, "desktop" }, std::pair{ showThemeButton, "theme" }, std::pair{ showOledButton, "oled" } })
            if (on) { if (!trayButtons.empty()) trayButtons += ','; trayButtons += key; }
    }
}

// Written whole or not at all (SaveFileWhole), one key a line, Windows line ends.
void Settings::Save() const {
    std::ostringstream f;
    for (auto& fd : kFields) {
        if (fd.legacy) continue;
        f << fd.key << "=";
        std::visit([&](auto m) {
            using T = std::remove_cvref_t<decltype(this->*m)>;
            if constexpr (std::is_same_v<T, std::string>) f << this->*m;
            else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int>) f << (int)(this->*m);
            else if constexpr (std::is_same_v<T, float>) f << this->*m;
            else f << (int)(this->*m);
        }, fd.member);
        f << "\r\n";
    }
    SaveFileWhole(AppDataDir() + L"\\settings.ini", f.str(), L"settings");
}

// A rename alone does not put the new file's data on the disk first, MOVEFILE_WRITE_THROUGH or not (that only
// flushes a move that copies): after a power loss the swapped-in name could hold zeros. And an ofstream's small
// file goes out only when it is closed, so its state before that never shows a full disk. Hence the handle.
bool SaveFileWhole(const std::wstring& path, const std::string& text, const wchar_t* what) {
    const std::wstring tmp = path + L".tmp";
    const size_t slash = path.find_last_of(L'\\');
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) &&
                    wrote == text.size() && FlushFileBuffers(f);
    const DWORD error = ok ? 0 : GetLastError();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        Log(L"%s: could not be written (error %lu): %s left as it was", what, error, name.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        Log(L"%s: could not replace %s (error %lu): left as it was", what, name.c_str(), e);
        return false;
    }
    return true;
}
