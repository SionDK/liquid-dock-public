// settings.ini (Settings::Load / Save) and dock.log (Log) on the dock's real code, in this test's own
// fixture folder: the executable lives in build\tests\dock-render, so AppDataDir() is
// build\tests\dock-render\data. Never touches the installed dock's data, the registry or other
// programs. One PASS/FAIL line per check, a summary line; the exit code is the number of failures.
#include "../src/common.h"
#include "portable_check.h"
#include <fstream>
#include <iterator>
#include <thread>
#include <atomic>
#include <cmath>
#include <set>

static std::wstring g_dir;

static std::string ReadAll(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
static void WriteAll(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}
static std::wstring Ini() { return g_dir + L"\\settings.ini"; }
static Settings LoadFrom(const std::string& text) { WriteAll(Ini(), text); Settings s; s.Load(); return s; }

static bool InRange(float v, float lo, float hi) { return v >= lo && v <= hi; }   // false for NaN

// Every option set to something other than its default.
static Settings NonDefault() {
    Settings s;
    s.edge = Edge::Right; s.iconSize = 58; s.magnification = 1.3f; s.glassStyle = 2;
    s.trayButtons = "oled,awake,screenshot,quiet,trash,downloads,audio";
    s.oledOneMonitor = true; s.trayIcons = true; s.fluentFlyout = false; s.clockSize = 1; s.iconSpacing = 2;
    s.iconGap = 37; s.dockThickness = 135; s.edgeGap = 22; s.edgeOffset = -45; s.autoHide = false;
    s.secondaryAlwaysShown = true; s.showWithStart = false; s.showDelay = 0.45f; s.hideDelay = 1.15f;
    s.autostart = true; s.startMenuExtras = false; s.shiftRunsAsAdmin = false; s.taskbarAppButtons = false;
    s.revealHotkey = 3; s.sensorSource = 2; s.adminWindows = 2;
    s.showCpuTemp = false; s.showGpu = false; s.showVram = false; s.showVramTemp = false; s.showRamTemp = false; s.showSsd = false;
    s.hwinfoCpuLabel = "\xEC\xBD\x94\xEC\x96\xB4 \xEC\x98\xA8\xEB\x8F\x84 (a=b)";   // "코어 온도 (a=b)": UTF-8, and an '=' in the value
    s.pinsVersion = 2;
    return s;
}
static bool Same(const Settings& a, const Settings& b) {
    return a.edge == b.edge && a.iconSize == b.iconSize && a.magnification == b.magnification && a.glassStyle == b.glassStyle &&
           a.trayButtons == b.trayButtons && a.oledOneMonitor == b.oledOneMonitor && a.trayIcons == b.trayIcons &&
           a.fluentFlyout == b.fluentFlyout && a.clockSize == b.clockSize && a.iconSpacing == b.iconSpacing &&
           a.iconGap == b.iconGap && a.dockThickness == b.dockThickness && a.edgeGap == b.edgeGap && a.edgeOffset == b.edgeOffset &&
           a.autoHide == b.autoHide && a.secondaryAlwaysShown == b.secondaryAlwaysShown && a.showWithStart == b.showWithStart &&
           a.showDelay == b.showDelay && a.hideDelay == b.hideDelay && a.autostart == b.autostart &&
           a.startMenuExtras == b.startMenuExtras && a.shiftRunsAsAdmin == b.shiftRunsAsAdmin &&
           a.taskbarAppButtons == b.taskbarAppButtons && a.revealHotkey == b.revealHotkey && a.sensorSource == b.sensorSource &&
           a.showCpuTemp == b.showCpuTemp && a.showGpu == b.showGpu && a.showVram == b.showVram && a.showVramTemp == b.showVramTemp &&
           a.showRamTemp == b.showRamTemp && a.showSsd == b.showSsd &&
           a.adminWindows == b.adminWindows && a.hwinfoCpuLabel == b.hwinfoCpuLabel && a.pinsVersion == b.pinsVersion;
}

static void SettingsChecks() {
    // ---- round trip of every option
    {
        Settings out = NonDefault(); out.Save();
        Settings in; in.Load();
        Check(Same(in, out), "every option survives save and a fresh load");
        Check(GetFileAttributesW((Ini() + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "save leaves no temporary file behind");
        std::string text = ReadAll(Ini());
        Check(text.find("showOledButton") == std::string::npos && text.find("showDesktopButton") == std::string::npos, "legacy switches are not written");
        Check(text.find("magnification=1.3\r\n") != std::string::npos && text.find("showDelay=0.45\r\n") != std::string::npos, "floats are written in short decimal form (CRLF lines)");
        Settings def; def.Save(); Settings back; back.Load();
        Check(Same(back, Settings{}), "the defaults survive save and load");
    }
    // ---- every slider position the settings window can store survives a round trip
    {
        bool all = true;
        for (int i = 0; i <= 11; ++i) {   // magnification 1.0 .. 2.1 step 0.1, as SetSlider rounds it
            Settings s; s.magnification = std::round((1.0f + i * 0.1f) * 1000) / 1000; s.Save();
            Settings b; b.Load(); all &= b.magnification == s.magnification;
        }
        for (int i = 0; i <= 12; ++i) {
            Settings s; s.showDelay = std::round((i * 0.05f) * 1000) / 1000; s.hideDelay = std::round((0.1f + i * 0.05f) * 1000) / 1000; s.Save();
            Settings b; b.Load(); all &= b.showDelay == s.showDelay && b.hideDelay == s.hideDelay;
        }
        Check(all, "slider float values round-trip exactly");
    }
    // ---- tolerant parsing
    {
        Settings s = LoadFrom("edge=1\r\niconSize=50\r\ntrayButtons=theme,oled\r\n");
        Check(s.edge == Edge::Left && s.iconSize == 50 && s.trayButtons == "theme,oled", "CRLF line ends are read");
        s = LoadFrom("bogus=1\nno equals sign\n=5\nedge=2\nedge=x\n");
        Check(s.edge == Edge::Right, "unknown keys and malformed lines are skipped, the last good value stays");
        s = LoadFrom("edge=9\niconSize=500\nglassStyle=-4\nrevealHotkey=77\nadminWindows=5\nedgeGap=999\n");
        Check(s.edge == Edge::Top && s.iconSize == 96 && s.glassStyle == 0 && s.revealHotkey == 3 && s.adminWindows == 2 && s.edgeGap == 40,
              "out-of-range numbers are clamped");
        s = LoadFrom("iconSize=inf\nshowDelay=-inf\n");
        Check(s.iconSize == 96 && s.showDelay == 0, "infinite values are clamped");
        s = LoadFrom("autoHide=0\ntrayIcons=7\nfluentFlyout=true\n");
        Check(!s.autoHide && s.trayIcons && s.fluentFlyout, "flags: 0 off, any other number on, words keep the default");
        s = LoadFrom("pinsVersion=99999999999999\n");
        Check(s.pinsVersion == 0, "a number too large for int keeps the default");
        s = LoadFrom("hwinfoCpuLabel=CPU (Tctl/Tdie)\n");
        Check(s.hwinfoCpuLabel == "CPU (Tctl/Tdie)", "text values are taken whole");
    }
    // NaN passes std::clamp unchanged ("nan" is what a NaN float is written as, and strtof reads it back), so
    // it is ignored like a malformed value (dock_render-1: showDelay=nan never let the dock come up at the edge).
    {
        Settings s = LoadFrom("iconSize=nan\nmagnification=nan\nshowDelay=nan\nhideDelay=-nan(ind)\n");
        Check(InRange(s.iconSize, 32, 96), "iconSize=nan is not taken (stays in 32..96)");
        Check(InRange(s.magnification, 1, 2.1f), "magnification=nan is not taken (stays in 1..2.1)");
        Check(InRange(s.showDelay, 0, 2), "showDelay=nan is not taken (stays in 0..2)");
        Check(InRange(s.hideDelay, 0, 3), "hideDelay=-nan(ind) is not taken (stays in 0..3)");
        s = LoadFrom("showDelay=0.4\nshowDelay=nan\n");
        Check(s.showDelay == 0.4f, "after showDelay=nan the last good value stays");
    }
    // ---- the button list: files from before it carry their three switches over
    {
        Settings s = LoadFrom("edge=0\niconSize=44\nshowDesktopButton=0\nshowThemeButton=1\nshowOledButton=1\npinsVersion=2\n");
        Check(s.trayButtons == "theme,oled", "the old switches become the button list");
        s = LoadFrom("edge=0\ntrayButtons=\n");
        Check(s.trayButtons.empty(), "an empty button list stays empty");
        s = LoadFrom("autostart=1\nshowDesktopButton=0\n");
        Check(s.trayButtons == "theme", "an old file with autostart and a switch is carried over too");
        s = LoadFrom("showOledButton=1\n");
        Check(s.trayButtons == "desktop,theme,oled", "a file with only an old switch is carried over");
    }
    // A first install: the installer writes only "autostart=1" (installer/setup_main.cpp), and a portable copy
    // starts with no file at all. Neither is "a file from before the button list": both keep the default list
    // (dock_render-2: they went through the migration and lost its SFF button).
    {
        const std::string fresh = Settings{}.trayButtons;
        Settings s = LoadFrom("autostart=1\n");
        Check(s.autostart && s.trayButtons == fresh, "the installer's first settings.ini gets the default button list");
        DeleteFileW(Ini().c_str());
        Settings none; none.Load();
        Check(none.trayButtons == fresh, "no settings.ini at all gets the default button list");
        s = LoadFrom("");
        Check(s.trayButtons == fresh, "an empty settings.ini gets the default button list");
    }
    // A UTF-8 byte order mark (an editor's "UTF-8 with BOM") is skipped: the dock's own files start with edge=
    // (dock_render-3: that first key was lost, and the next save made the loss stick).
    {
        Settings s = LoadFrom("\xEF\xBB\xBF" "edge=1\niconSize=50\nhwinfoCpuLabel=\xEC\xBD\x94\xEC\x96\xB4 \xEC\x98\xA8\xEB\x8F\x84\n");
        Check(s.iconSize == 50 && s.hwinfoCpuLabel == "\xEC\xBD\x94\xEC\x96\xB4 \xEC\x98\xA8\xEB\x8F\x84", "keys after a BOM line are read");
        Check(s.edge == Edge::Left, "the first key after a UTF-8 BOM is read");
        s.Save();
        Check(ReadAll(Ini()).rfind("edge=1\r\n", 0) == 0, "and saved again without the mark");
        s = LoadFrom("\xEF\xBB\xBF" "\nedge=2\n");
        Check(s.edge == Edge::Right, "a BOM on an empty first line");
        s = LoadFrom("edge=1\n\xEF\xBB\xBF" "iconSize=50\n");
        Check(s.edge == Edge::Left && s.iconSize == 44, "a mark inside the file is not a BOM: that line is skipped");
    }
    // ---- a failed save never damages the file that is there
    {
        Settings keep = NonDefault(); keep.Save();
        const std::string before = ReadAll(Ini());
        const std::wstring tmp = Ini() + L".tmp";
        CreateDirectoryW(tmp.c_str(), nullptr);   // the temporary file cannot be created
        Settings other; other.edge = Edge::Top; other.Save();
        Check(ReadAll(Ini()) == before, "when the temporary file cannot be written the old file stays");
        RemoveDirectoryW(tmp.c_str());
        SetFileAttributesW(Ini().c_str(), FILE_ATTRIBUTE_READONLY);
        other.Save();
        Check(ReadAll(Ini()) == before, "a read-only settings.ini is left as it is");
        SetFileAttributesW(Ini().c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(tmp.c_str());
        other.Save();
        Settings back; back.Load();
        Check(back.edge == Edge::Top, "the next save after the obstacle is gone goes through");
        // a temporary file another handle holds with every sharing and locks all: an ofstream opens it and its
        // data cannot go out, as on a full disk - it found out only at its close, after its check, and the empty
        // file was swapped in (apps_model-7)
        const std::string kept = ReadAll(Ini());
        HANDLE hold = CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        const bool locked = hold != INVALID_HANDLE_VALUE && LockFile(hold, 0, 0, MAXDWORD, MAXDWORD);
        Settings third; third.edge = Edge::Right; third.Save();
        Check(locked && ReadAll(Ini()) == kept && !kept.empty(), "a temporary file whose data cannot be written never replaces the old file");
        if (hold != INVALID_HANDLE_VALUE) { UnlockFile(hold, 0, 0, MAXDWORD, MAXDWORD); CloseHandle(hold); }
        DeleteFileW(tmp.c_str());
        third.Save();
        back.Load();
        Check(back.edge == Edge::Right && GetFileAttributesW(tmp.c_str()) == INVALID_FILE_ATTRIBUTES,
              "...and the next save goes through, no temporary file left");
    }
}

// ---------------------------------------------------------------- dock.log

static std::wstring LogPath() { return g_dir + L"\\dock.log"; }

// This executable again, as a child process in the given mode; its exit code.
static int RunSelf(const std::wstring& exe, const wchar_t* mode) {
    std::wstring cmd = L"\"" + exe + L"\" " + mode;
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

static void LogChecks(const wchar_t* exe) {
    // The log is opened once per process, so each case runs in a child process of this executable.
    auto child = [&](const wchar_t* mode) { return RunSelf(exe, mode); };
    auto lines = [&] {
        std::string all = ReadAll(LogPath());
        if (all.rfind("\xEF\xBB\xBF", 0) == 0) all.erase(0, 3);
        std::vector<std::string> out; size_t p = 0;
        while (p < all.size()) {   // written in text mode: CRLF
            size_t e = all.find('\n', p); if (e == std::string::npos) e = all.size();
            std::string l = all.substr(p, e - p); if (!l.empty() && l.back() == '\r') l.pop_back();
            out.push_back(l); p = e + 1;
        }
        return out;
    };
    auto text = [](const std::string& line) { return line.size() > 13 ? line.substr(13) : std::string(); };   // after "HH:MM:SS.mmm "

    Check(child(L"--log-basic") == 0, "child process logged");
    {
        std::string all = ReadAll(LogPath());
        Check(all.rfind("\xEF\xBB\xBF", 0) == 0, "dock.log is UTF-8 with a byte order mark");
        auto ls = lines();
        Check(ls.size() == 3 && text(ls[0]) == "first line" && text(ls[1]) == "\xED\x95\x9C\xEA\xB8\x80 42 \xC2\xB0" && text(ls[2]) == "x%y",
              "lines carry a time stamp and their text, Korean in UTF-8");
        bool stamped = !ls.empty() && ls[0].size() > 13 && ls[0][2] == ':' && ls[0][5] == ':' && ls[0][8] == '.' && ls[0][12] == ' ';
        Check(stamped, "time stamp format HH:MM:SS.mmm");
    }
    Check(child(L"--log-second") == 0, "second child process logged");
    {
        auto ls = lines();
        Check(ls.size() == 1 && text(ls[0]) == "second run", "dock.log is rewritten at every start");
    }
    Check(child(L"--log-threads") == 0, "threaded child logged");
    {
        auto ls = lines();
        std::set<std::string> seen; bool wellFormed = true;
        for (auto& l : ls) { auto t = text(l); if (t.rfind("thread ", 0) != 0) wellFormed = false; seen.insert(t); }
        Check(ls.size() == 8 * 400 && seen.size() == 8 * 400 && wellFormed, "8 threads x 400 lines: every line whole, none lost or mixed");
    }
    Check(child(L"--log-long") == 0, "long-line child logged");
    {
        auto ls = lines();
        Check(ls.size() == 2 && text(ls[0]).size() == 2047 - 0 && text(ls[1]) == "after the long one", "a line longer than the buffer is cut at 2047 characters");
    }
    Check(child(L"--log-cap") == 0, "cap child logged");
    {
        auto ls = lines();
        Check(ls.size() == 20000, "the log stops at 20000 lines however long the dock runs");
        Check(GetFileAttributesW(LogPath().c_str()) != INVALID_FILE_ATTRIBUTES && ReadAll(LogPath()).size() < 2u * 1024 * 1024, "so the file stays small");
    }
    Check(child(L"--log-surrogate") == 0, "surrogate child logged");
    {
        auto ls = lines();
        Check(ls.size() == 3 && text(ls[2]) == "after the odd one", "a lone UTF-16 surrogate (a damaged file name) does not stop later lines");
    }
    // The exe's other runs call LogAppend first: the restore shortcut once the dock has ended ("oled: off"), an
    // elevated copy that hands over to the user's (dock_render-8: their first line emptied the file).
    Check(child(L"--log-basic") == 0 && child(L"--log-append") == 0, "a dock run, then a run that adds to its log");
    {
        auto ls = lines();
        const std::string all = ReadAll(LogPath());
        Check(ls.size() == 4 && text(ls[0]) == "first line" && text(ls[3]) == "oled: off", "a run that adds to the log keeps the session before it");
        Check(all.rfind("\xEF\xBB\xBF", 0) == 0 && all.find("\xEF\xBB\xBF", 3) == std::string::npos, "...and the file keeps one byte order mark");
    }
    {   // emptied (this process keeps its own dock.log open since a failed save above logged, so it cannot be deleted)
        HANDLE f = CreateFileW(LogPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, TRUNCATE_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    Check(child(L"--log-append") == 0, "a run that adds to an empty log");
    {
        auto ls = lines();
        Check(ReadAll(LogPath()).rfind("\xEF\xBB\xBF", 0) == 0 && ls.size() == 1 && text(ls[0]) == "oled: off", "...starts it, with its byte order mark");
    }
    Check(child(L"--log-handover") == 0, "a run that adds lines, then goes on as the dock");
    {   // the session before it ("oled: off") goes; why this run stays elevated, logged before the mutex, stays
        auto ls = lines();
        const std::string all = ReadAll(LogPath());
        Check(ls.size() == 2 && text(ls[0]) == "started as administrator; starting as the user failed: staying elevated" && text(ls[1]) == "the dock's session",
              "the dock's session starts the log afresh, with this run's lines from before it at the top");
        Check(all.rfind("\xEF\xBB\xBF", 0) == 0 && all.find("\xEF\xBB\xBF", 3) == std::string::npos, "...and one byte order mark");
    }
    Check(child(L"--log-handover-many") == 0, "a run that adds many lines, then goes on as the dock");
    {
        auto ls = lines();
        Check(ls.size() == 17 && text(ls[0]) == "before 0" && text(ls[15]) == "before 15" && text(ls[16]) == "the dock's session",
              "at most 16 lines from before the session are kept for it");
    }
    Check(child(L"--log-basic") == 0 && child(L"--log-locked") == 0, "a dock run, then a dock that cannot open dock.log at first");
    SetFileAttributesW(LogPath().c_str(), FILE_ATTRIBUTE_NORMAL);   // in case the child did not get that far
    {   // read-only stands for a program that holds the file without write sharing: the same failed open
        auto ls = lines();
        Check(ls.size() == 3 && text(ls[0]) == "before the session" && text(ls[1]) == "while dock.log cannot be opened" && text(ls[2]) == "the dock's session",
              "a session whose fresh start failed starts afresh once dock.log opens, its lines so far at the top");
    }
    Check(child(L"--log-while-running") == 0, "another run adds a line while the dock writes");
    {
        auto ls = lines();
        Check(ls.size() == 3 && text(ls[0]) == "dock before" && text(ls[1]) == "added by another run" && text(ls[2]) == "dock after",
              "a line another run adds while the dock writes is not overwritten by the dock's next one");
    }
}

static int Child(const std::wstring& mode) {
    if (mode == L"--log-basic") { Log(L"first line"); Log(L"%s %d %s", L"한글", 42, L"°"); Log(L"x%%y"); return 0; }
    if (mode == L"--log-second") { Log(L"second run"); return 0; }
    if (mode == L"--log-threads") {
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t) ts.emplace_back([t] { for (int i = 0; i < 400; ++i) Log(L"thread %d line %d with some padding text", t, i); });
        for (auto& t : ts) t.join();
        return 0;
    }
    if (mode == L"--log-long") { std::wstring big(5000, L'a'); Log(L"%s", big.c_str()); Log(L"after the long one"); return 0; }
    if (mode == L"--log-cap") { for (int i = 0; i < 20500; ++i) Log(L"line %d", i); return 0; }
    if (mode == L"--log-surrogate") { Log(L"before"); const wchar_t odd[] = { L'f', 0xD800, L'.', L't', L'x', L't', 0 }; Log(L"file %s", odd); Log(L"after the odd one"); return 0; }
    if (mode == L"--log-append") { LogAppend(); Log(L"oled: off"); return 0; }
    if (mode == L"--log-handover") {   // RestartUnelevated failed: staying elevated, this run becomes the dock
        LogAppend(); Log(L"started as administrator; starting as the user failed: staying elevated");
        LogStartSession(); Log(L"the dock's session");
        return 0;
    }
    if (mode == L"--log-handover-many") {
        LogAppend();
        for (int i = 0; i < 20; ++i) Log(L"before %d", i);
        LogStartSession(); Log(L"the dock's session");
        return 0;
    }
    if (mode == L"--log-locked") {   // the session's fresh start fails; the next line after the file opens again starts it
        const std::wstring path = AppDataDir() + L"\\dock.log";
        if (!SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY)) return 5;
        LogAppend(); Log(L"before the session");
        LogStartSession(); Log(L"while dock.log cannot be opened");
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        Log(L"the dock's session");
        return 0;
    }
    if (mode == L"--log-while-running") {
        LogStartSession(); Log(L"dock before");
        wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (RunSelf(exe, L"--log-added") != 0) return 4;
        Log(L"dock after");
        return 0;
    }
    if (mode == L"--log-added") { LogAppend(); Log(L"added by another run"); return 0; }
    return 3;
}

int wmain(int argc, wchar_t** argv) {
    g_dir = AppDataDir();
    // the fixture folder only: never the installed dock's data
    if (g_dir.find(L"\\tests\\dock-render\\") == std::wstring::npos) { printf("not in the fixture folder\n"); return 100; }
    if (argc > 1) return Child(argv[1]);
    DeleteFileW(Ini().c_str());
    SettingsChecks();
    LogChecks(argv[0]);
    DeleteFileW(Ini().c_str());
    return Summary("dock_render_settings");
}
