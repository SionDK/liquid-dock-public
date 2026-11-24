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
    s.oledOneMonitor = true; s.trayIcons = true; s.fluentFlyout = false; s.clockSize = 1; s.clockFormat = 1; s.iconSpacing = 2;
    s.iconGap = 37; s.dockThickness = 135; s.edgeGap = 22; s.edgeOffset = -45; s.autoHide = false;
    s.secondaryAlwaysShown = true; s.showWithStart = false; s.showDelay = 0.45f; s.hideDelay = 1.15f;
    s.autostart = true; s.startMenuExtras = false; s.shiftRunsAsAdmin = false; s.taskbarAppButtons = false;
    s.revealHotkey = 3; s.sensorSource = 2; s.adminWindows = 2;
    s.showCpuTemp = false; s.showGpu = false; s.showVram = false; s.showVramTemp = false; s.showRamTemp = false; s.showSsd = false;
    s.powerView = 2; s.showCpuPower = false; s.showGpuPower = false;
    s.hwinfoCpuLabel = "\xEC\xBD\x94\xEC\x96\xB4 \xEC\x98\xA8\xEB\x8F\x84 (a=b)";   // "코어 온도 (a=b)": UTF-8, and an '=' in the value
    s.pinsVersion = 2;
    return s;
}
static bool Same(const Settings& a, const Settings& b) {
    return a.edge == b.edge && a.iconSize == b.iconSize && a.magnification == b.magnification && a.glassStyle == b.glassStyle &&
           a.trayButtons == b.trayButtons && a.oledOneMonitor == b.oledOneMonitor && a.trayIcons == b.trayIcons &&
           a.fluentFlyout == b.fluentFlyout && a.clockSize == b.clockSize && a.clockFormat == b.clockFormat && a.iconSpacing == b.iconSpacing &&
           a.iconGap == b.iconGap && a.dockThickness == b.dockThickness && a.edgeGap == b.edgeGap && a.edgeOffset == b.edgeOffset &&
           a.autoHide == b.autoHide && a.secondaryAlwaysShown == b.secondaryAlwaysShown && a.showWithStart == b.showWithStart &&
           a.showDelay == b.showDelay && a.hideDelay == b.hideDelay && a.autostart == b.autostart &&
           a.startMenuExtras == b.startMenuExtras && a.shiftRunsAsAdmin == b.shiftRunsAsAdmin &&
           a.taskbarAppButtons == b.taskbarAppButtons && a.revealHotkey == b.revealHotkey && a.sensorSource == b.sensorSource &&
           a.showCpuTemp == b.showCpuTemp && a.showGpu == b.showGpu && a.showVram == b.showVram && a.showVramTemp == b.showVramTemp &&
           a.showRamTemp == b.showRamTemp && a.showSsd == b.showSsd &&
           a.powerView == b.powerView && a.showCpuPower == b.showCpuPower && a.showGpuPower == b.showGpuPower &&
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

// ---------------------------------------------------------------- settings.ini golden bytes
// What the dock writes, and what it makes of the files older versions wrote, pinned byte for byte (the round trip
// above passes when writing and reading change together). Every value below is what the code did at mod.35.

// Every member, the read-only legacy ones and sourceFromCpuTemp too; floats with every digit.
static std::string Dump(const Settings& s) {
    std::string d;
    auto i = [&](const char* k, int v) { d += k; d += '='; d += std::to_string(v); d += ' '; };
    auto f = [&](const char* k, float v) { char b[48]; snprintf(b, sizeof b, "%s=%.9g ", k, v); d += b; };
    auto t = [&](const char* k, const std::string& v) { d += k; d += "=["; d += v; d += "] "; };
    i("edge", (int)s.edge); f("iconSize", s.iconSize); f("magnification", s.magnification); i("glassStyle", s.glassStyle);
    t("trayButtons", s.trayButtons);
    i("showDesktopButton", s.showDesktopButton); i("showThemeButton", s.showThemeButton); i("showOledButton", s.showOledButton);
    i("oledOneMonitor", s.oledOneMonitor); i("trayIcons", s.trayIcons); i("fluentFlyout", s.fluentFlyout);
    i("clockSize", s.clockSize); i("iconSpacing", s.iconSpacing); i("iconGap", s.iconGap); i("dockThickness", s.dockThickness);
    i("edgeGap", s.edgeGap); i("edgeOffset", s.edgeOffset); i("autoHide", s.autoHide); i("secondaryAlwaysShown", s.secondaryAlwaysShown);
    i("showWithStart", s.showWithStart); f("showDelay", s.showDelay); f("hideDelay", s.hideDelay); i("autostart", s.autostart);
    i("startMenuExtras", s.startMenuExtras); i("shiftRunsAsAdmin", s.shiftRunsAsAdmin); i("taskbarAppButtons", s.taskbarAppButtons);
    i("revealHotkey", s.revealHotkey); i("sensorSource", s.sensorSource); i("cpuTemp", s.cpuTemp); i("sourceFromCpuTemp", s.sourceFromCpuTemp);
    i("showCpuTemp", s.showCpuTemp); i("showGpu", s.showGpu); i("showVram", s.showVram); i("showVramTemp", s.showVramTemp);
    i("showRamTemp", s.showRamTemp); i("showSsd", s.showSsd);
    i("powerView", s.powerView); i("showCpuPower", s.showCpuPower); i("showGpuPower", s.showGpuPower);
    i("adminWindows", s.adminWindows);
    t("hwinfoCpuLabel", s.hwinfoCpuLabel); i("pinsVersion", s.pinsVersion);
    return d;
}
static std::string Escaped(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '\r') o += "\\r"; else if (c == '\n') o += "\\n"; else if (c == '\t') o += "\\t";
        else if (c < 0x20 || c >= 0x7F) { char b[8]; snprintf(b, sizeof b, "\\x%02X", c); o += b; } else o += (char)c;
    }
    return o;
}
// on a failure, what came out (only then: a passing run prints its PASS line alone)
static void CheckSame(const std::string& got, const std::string& want, const char* what) {
    if (!Check(got == want, what)) printf("  got:  %s\n  want: %s\n", Escaped(got).c_str(), Escaped(want).c_str());
}
static std::string SavedText(const Settings& s) { s.Save(); return ReadAll(Ini()); }
static Settings LoadInto(Settings s, const std::string& text) { WriteAll(Ini(), text); s.Load(); return s; }

// The default file as mod.21 base .. mod.32 wrote it (the same 28 keys, cpuTemp still
// written, no sensorSource or show* keys, CRLF lines), with the values of a test PC's dock.
static const char kOldFile[] =
    "edge=0\r\niconSize=44\r\nmagnification=1.7\r\nglassStyle=1\r\ntrayButtons=desktop,theme,oled,sff\r\noledOneMonitor=0\r\n"
    "trayIcons=1\r\nfluentFlyout=1\r\nclockSize=0\r\nclockFormat=0\r\nsensorSize=0\r\niconSpacing=1\r\niconGap=0\r\ndockThickness=100\r\nedgeGap=6\r\n"
    "edgeOffset=-10\r\nautoHide=1\r\nsecondaryAlwaysShown=0\r\nshowWithStart=1\r\nshowDelay=0.05\r\nhideDelay=0.35\r\n"
    "autostart=1\r\nstartMenuExtras=1\r\nshiftRunsAsAdmin=1\r\ntaskbarAppButtons=1\r\nrevealHotkey=0\r\ncpuTemp=1\r\n"
    "adminWindows=1\r\nhwinfoCpuLabel=\r\npinsVersion=2\r\n";

static void GoldenChecks() {
    // ---- (a) every written field away from its default, in the number forms the stream gives
    {
        Settings s;
        s.edge = Edge::Top; s.iconSize = 52.5f; s.magnification = 1.23456789f; s.glassStyle = 0; s.trayButtons = "sff,oled,desktop";
        s.showDesktopButton = false; s.showThemeButton = false; s.showOledButton = true;   // legacy: not written
        s.oledOneMonitor = true; s.trayIcons = true; s.fluentFlyout = false; s.clockSize = 1; s.iconSpacing = 0; s.iconGap = 60;
        s.dockThickness = 75; s.edgeGap = 0; s.edgeOffset = -100; s.autoHide = false; s.secondaryAlwaysShown = true;
        s.showWithStart = false; s.showDelay = 0.00001f; s.hideDelay = 3.0f; s.autostart = true; s.startMenuExtras = false;
        s.shiftRunsAsAdmin = false; s.taskbarAppButtons = false; s.revealHotkey = 1; s.sensorSource = 0;
        s.cpuTemp = true; s.sourceFromCpuTemp = true;   // not written
        s.showCpuTemp = false; s.showGpu = false; s.showVram = false; s.showVramTemp = false; s.showRamTemp = false; s.showSsd = false;
        s.powerView = 1; s.showCpuPower = false; s.showGpuPower = false;
        s.adminWindows = 1; s.hwinfoCpuLabel = "GPU \xEB\xA9\x94\xEB\xAA\xA8\xEB\xA6\xAC =x ";   // "GPU 메모리 =x ": a trailing blank
        s.pinsVersion = 1000000;
        CheckSame(SavedText(s),
            "edge=3\r\niconSize=52.5\r\nmagnification=1.23457\r\nglassStyle=0\r\ntrayButtons=sff,oled,desktop\r\noledOneMonitor=1\r\n"
            "trayIcons=1\r\nfluentFlyout=0\r\nclockSize=1\r\nclockFormat=0\r\nsensorSize=0\r\niconSpacing=0\r\niconGap=60\r\ndockThickness=75\r\nedgeGap=0\r\n"
            "edgeOffset=-100\r\nautoHide=0\r\nsecondaryAlwaysShown=1\r\nshowWithStart=0\r\nshowDelay=1e-05\r\nhideDelay=3\r\n"
            "autostart=1\r\nstartMenuExtras=0\r\nshiftRunsAsAdmin=0\r\ntaskbarAppButtons=0\r\nrevealHotkey=1\r\nsensorSource=0\r\n"
            "showCpuTemp=0\r\nshowGpu=0\r\nshowVram=0\r\nshowVramTemp=0\r\nshowRamTemp=0\r\nshowSsd=0\r\npowerView=1\r\nshowCpuPower=0\r\nshowGpuPower=0\r\n"
            "adminWindows=1\r\nhwinfoCpuLabel=GPU \xEB\xA9\x94\xEB\xAA\xA8\xEB\xA6\xAC =x \r\npinsVersion=1000000\r\n",
            "golden: every field off its default is written whole: key order, CRLF, 6 significant digits, 1e-05, no legacy key");
        CheckSame(SavedText(NonDefault()),
            "edge=2\r\niconSize=58\r\nmagnification=1.3\r\nglassStyle=2\r\ntrayButtons=oled,awake,screenshot,quiet,trash,downloads,audio\r\n"
            "oledOneMonitor=1\r\ntrayIcons=1\r\nfluentFlyout=0\r\nclockSize=1\r\nclockFormat=1\r\nsensorSize=0\r\niconSpacing=2\r\niconGap=37\r\ndockThickness=135\r\n"
            "edgeGap=22\r\nedgeOffset=-45\r\nautoHide=0\r\nsecondaryAlwaysShown=1\r\nshowWithStart=0\r\nshowDelay=0.45\r\nhideDelay=1.15\r\n"
            "autostart=1\r\nstartMenuExtras=0\r\nshiftRunsAsAdmin=0\r\ntaskbarAppButtons=0\r\nrevealHotkey=3\r\nsensorSource=2\r\n"
            "showCpuTemp=0\r\nshowGpu=0\r\nshowVram=0\r\nshowVramTemp=0\r\nshowRamTemp=0\r\nshowSsd=0\r\npowerView=2\r\nshowCpuPower=0\r\nshowGpuPower=0\r\n"
            "adminWindows=2\r\n"
            "hwinfoCpuLabel=\xEC\xBD\x94\xEC\x96\xB4 \xEC\x98\xA8\xEB\x8F\x84 (a=b)\r\npinsVersion=2\r\n",
            "golden: the round-trip fixture's file, byte for byte");
        // Save takes what it is given: no clamp, and the stream's forms for large, NaN and infinite numbers
        Settings odd;
        odd.edge = (Edge)7; odd.iconSize = 1234567.0f; odd.magnification = std::nanf(""); odd.showDelay = INFINITY; odd.hideDelay = -0.0f;
        odd.iconGap = -3; odd.pinsVersion = 2000000; odd.trayButtons = "";
        const std::string t = SavedText(odd);
        CheckSame(t.substr(0, t.find("glassStyle")), "edge=7\r\niconSize=1.23457e+06\r\nmagnification=nan\r\n",
                  "golden: out-of-range values are written as they are (edge=7, 1.23457e+06, nan)");
        Check(t.find("\r\ntrayButtons=\r\n") != std::string::npos && t.find("\r\niconGap=-3\r\n") != std::string::npos &&
              t.find("\r\nshowDelay=inf\r\nhideDelay=-0\r\n") != std::string::npos && t.find("\r\npinsVersion=2000000\r\n") != std::string::npos,
              "golden: an empty list, -3, inf, -0 and 2000000 are written as they are");
    }
    // ---- (b) the defaults: the file a dock writes on its first save
    {
        const std::string def =
            "edge=0\r\niconSize=44\r\nmagnification=1.7\r\nglassStyle=1\r\ntrayButtons=desktop,theme,sff\r\noledOneMonitor=0\r\n"
            "trayIcons=0\r\nfluentFlyout=1\r\nclockSize=0\r\nclockFormat=0\r\nsensorSize=0\r\niconSpacing=1\r\niconGap=0\r\ndockThickness=100\r\nedgeGap=6\r\n"
            "edgeOffset=0\r\nautoHide=1\r\nsecondaryAlwaysShown=0\r\nshowWithStart=1\r\nshowDelay=0.05\r\nhideDelay=0.35\r\n"
            "autostart=0\r\nstartMenuExtras=1\r\nshiftRunsAsAdmin=1\r\ntaskbarAppButtons=1\r\nrevealHotkey=0\r\nsensorSource=1\r\n"
            "showCpuTemp=1\r\nshowGpu=1\r\nshowVram=1\r\nshowVramTemp=1\r\nshowRamTemp=1\r\nshowSsd=1\r\npowerView=0\r\nshowCpuPower=1\r\nshowGpuPower=1\r\n"
            "adminWindows=0\r\n"
            "hwinfoCpuLabel=\r\npinsVersion=0\r\n";
        CheckSame(SavedText(Settings{}), def, "golden: the default settings' file, byte for byte");
        CheckSame(Dump(Settings{}),
            "edge=0 iconSize=44 magnification=1.70000005 glassStyle=1 trayButtons=[desktop,theme,sff] showDesktopButton=1 "
            "showThemeButton=1 showOledButton=0 oledOneMonitor=0 trayIcons=0 fluentFlyout=1 clockSize=0 iconSpacing=1 iconGap=0 "
            "dockThickness=100 edgeGap=6 edgeOffset=0 autoHide=1 secondaryAlwaysShown=0 showWithStart=1 showDelay=0.0500000007 "
            "hideDelay=0.349999994 autostart=0 startMenuExtras=1 shiftRunsAsAdmin=1 taskbarAppButtons=1 revealHotkey=0 sensorSource=1 "
            "cpuTemp=0 sourceFromCpuTemp=0 showCpuTemp=1 showGpu=1 showVram=1 showVramTemp=1 showRamTemp=1 showSsd=1 "
            "powerView=0 showCpuPower=1 showGpuPower=1 adminWindows=0 "
            "hwinfoCpuLabel=[] pinsVersion=0 ",
            "golden: every default, the legacy switches too");
        // the installer's first file (installer/setup_main.cpp: "autostart=1\n" and nothing else), then the dock's first save
        Settings first = LoadFrom("autostart=1\n");
        Settings want; want.autostart = true;
        CheckSame(Dump(first), Dump(want), "the installer's first file: autostart on, every other field its default");
        std::string afterInstall = def; afterInstall.replace(afterInstall.find("autostart=0"), 11, "autostart=1");
        CheckSame(SavedText(first), afterInstall, "...and the dock's first save of it is the default file with autostart=1");
        // the default file read back gives exactly the defaults (nothing migrated, no sourceFromCpuTemp)
        CheckSame(Dump(LoadFrom(def)), Dump(Settings{}), "the default file read back: every field its default");
    }
    // ---- (c) files older versions wrote, and the compatibility marks (d)
    {
        // mod.21 base .. mod.32 (cpuTemp=1: PawnIO was on): sensorSource 2 taken from it, marked for SensorClient::Start
        Settings s = LoadFrom(kOldFile);
        Settings e; e.trayButtons = "desktop,theme,oled,sff"; e.trayIcons = true; e.edgeOffset = -10; e.autostart = true;
        e.cpuTemp = true; e.adminWindows = 1; e.pinsVersion = 2; e.sensorSource = 2; e.sourceFromCpuTemp = true;
        CheckSame(Dump(s), Dump(e), "a mod.21..mod.32 file with cpuTemp=1: every field, sensorSource 2 and sourceFromCpuTemp");
        CheckSame(SavedText(s),
            "edge=0\r\niconSize=44\r\nmagnification=1.7\r\nglassStyle=1\r\ntrayButtons=desktop,theme,oled,sff\r\noledOneMonitor=0\r\n"
            "trayIcons=1\r\nfluentFlyout=1\r\nclockSize=0\r\nclockFormat=0\r\nsensorSize=0\r\niconSpacing=1\r\niconGap=0\r\ndockThickness=100\r\nedgeGap=6\r\n"
            "edgeOffset=-10\r\nautoHide=1\r\nsecondaryAlwaysShown=0\r\nshowWithStart=1\r\nshowDelay=0.05\r\nhideDelay=0.35\r\n"
            "autostart=1\r\nstartMenuExtras=1\r\nshiftRunsAsAdmin=1\r\ntaskbarAppButtons=1\r\nrevealHotkey=0\r\nsensorSource=2\r\n"
            "showCpuTemp=1\r\nshowGpu=1\r\nshowVram=1\r\nshowVramTemp=1\r\nshowRamTemp=1\r\nshowSsd=1\r\npowerView=0\r\nshowCpuPower=1\r\nshowGpuPower=1\r\n"
            "adminWindows=1\r\n"
            "hwinfoCpuLabel=\r\npinsVersion=2\r\n",
            "...saved again: the cpuTemp line becomes sensorSource=2, the six show lines and the three power lines (mod.40), nothing else changes");
        std::string off = kOldFile; off.replace(off.find("cpuTemp=1"), 9, "cpuTemp=0");
        s = LoadFrom(off);
        e.cpuTemp = false; e.sensorSource = 1; e.sourceFromCpuTemp = false;
        CheckSame(Dump(s), Dump(e), "a mod.21..mod.32 file with cpuTemp=0: sensorSource 1 (HWiNFO), not marked");
        // the same keys in LF lines (a file edited by hand)
        std::string lf; for (char c : std::string(kOldFile)) if (c != '\r') lf += c;
        e.cpuTemp = true; e.sensorSource = 2; e.sourceFromCpuTemp = true;
        CheckSame(Dump(LoadFrom(lf)), Dump(e), "the same file with LF lines reads the same");
        // CRLF and LF mixed, the last line without a line end
        std::string mixed = kOldFile; mixed.replace(mixed.find("glassStyle=1\r\n"), 14, "glassStyle=1\n");
        mixed.replace(mixed.find("pinsVersion=2\r\n"), 15, "pinsVersion=2");
        CheckSame(Dump(LoadFrom(mixed)), Dump(e), "CRLF and LF mixed, and no line end after the last line: read the same");
        // a UTF-8 byte order mark before an old file, CRLF lines
        CheckSame(Dump(LoadFrom(std::string("\xEF\xBB\xBF") + kOldFile)), Dump(e), "an old file saved with a UTF-8 BOM: read the same");
        // before the button list (the three switches, no trayButtons line), cpuTemp=1
        s = LoadFrom("edge=3\r\niconSize=40\r\nmagnification=1.5\r\nglassStyle=2\r\nshowDesktopButton=1\r\nshowThemeButton=0\r\n"
                     "showOledButton=1\r\nautoHide=0\r\nshowDelay=0.2\r\nhideDelay=0.5\r\nautostart=1\r\ncpuTemp=1\r\npinsVersion=2\r\n");
        Settings b; b.edge = Edge::Top; b.iconSize = 40; b.magnification = 1.5f; b.glassStyle = 2; b.showThemeButton = false;
        b.showOledButton = true; b.trayButtons = "desktop,oled"; b.autoHide = false; b.showDelay = 0.2f; b.hideDelay = 0.5f;
        b.autostart = true; b.cpuTemp = true; b.pinsVersion = 2; b.sensorSource = 2; b.sourceFromCpuTemp = true;
        CheckSame(Dump(s), Dump(b), "a file from before the button list: its switches become the list, every field");
        // cpuTemp alone is a dock key (only autostart is not): the button list is rebuilt from the switches' defaults
        s = LoadFrom("cpuTemp=1\n");
        Settings c1; c1.cpuTemp = true; c1.sensorSource = 2; c1.sourceFromCpuTemp = true; c1.trayButtons = "desktop,theme";
        CheckSame(Dump(s), Dump(c1), "cpuTemp=1 alone: PawnIO marked, and the button list rebuilt as desktop,theme");
        s = LoadFrom("cpuTemp=0\n");
        Settings c0; c0.trayButtons = "desktop,theme";
        CheckSame(Dump(s), Dump(c0), "cpuTemp=0 alone: HWiNFO, not marked, the list rebuilt too");
        s = LoadFrom("cpuTemp=x\ntrayButtons=sff\n");
        Settings cx; cx.trayButtons = "sff";
        CheckSame(Dump(s), Dump(cx), "cpuTemp=x (malformed): HWiNFO, not marked");
        s = LoadFrom("cpuTemp=5\ntrayButtons=sff\n");
        cx.cpuTemp = true; cx.sensorSource = 2; cx.sourceFromCpuTemp = true;
        CheckSame(Dump(s), Dump(cx), "cpuTemp=5: on, PawnIO marked");
        // a file with both: sensorSource wins, wherever it stands, and nothing is marked
        Settings both; both.trayButtons = "sff"; both.cpuTemp = true; both.sensorSource = 0;
        CheckSame(Dump(LoadFrom("cpuTemp=1\nsensorSource=0\ntrayButtons=sff\n")), Dump(both), "cpuTemp=1 then sensorSource=0: source 0, not marked");
        CheckSame(Dump(LoadFrom("sensorSource=0\ncpuTemp=1\ntrayButtons=sff\n")), Dump(both), "sensorSource=0 then cpuTemp=1: the same");
        Settings bad; bad.trayButtons = "sff"; bad.cpuTemp = true;
        CheckSame(Dump(LoadFrom("sensorSource=x\ncpuTemp=1\ntrayButtons=sff\n")), Dump(bad),
                  "a malformed sensorSource line still counts as seen: source stays 1, cpuTemp=1 not marked");
        // only the six show keys of mod.33 without sensorSource: source 1
        Settings sh; sh.showGpu = false; sh.trayButtons = "desktop,theme";
        CheckSame(Dump(LoadFrom("showGpu=0\n")), Dump(sh), "showGpu=0 alone: source 1, the list rebuilt from the switches' defaults");
    }
    // ---- unknown keys, odd lines, numbers in odd forms, every range
    {
        Settings s = LoadFrom("Edge=2\n edge=2\nedge =2\nEDGE=2\nunknown=1\nshowDesktopButton2=0\n#edge=2\n;edge=2\n[Settings]\n");
        Settings e;   // no line is a known key: a fresh install's defaults
        CheckSame(Dump(s), Dump(e), "keys are matched exactly: case, blanks, comments and sections are unknown lines");
        s = LoadFrom("edge= 2\nglassStyle=2abc\nrevealHotkey=1.9\niconSize=0x40\nmagnification=1,5\npinsVersion=0x10\n"
                     "showDelay= .25\nhideDelay=1e0\nclockSize=+1\ntrayButtons=a=b \n");
        Settings f; f.edge = Edge::Right; f.glassStyle = 2; f.revealHotkey = 1; f.iconSize = 64; f.magnification = 1;
        f.pinsVersion = 0; f.showDelay = 0.25f; f.hideDelay = 1; f.clockSize = 1; f.trayButtons = "a=b ";
        CheckSame(Dump(s), Dump(f), "number forms: a leading blank, trailing junk, 1.9 as int, hex float, comma, +1, text after '=' whole");
        s = LoadFrom("autoHide=-1\nshowWithStart= 0\nautostart=00\ntrayIcons=1x\nfluentFlyout=\nstartMenuExtras=0.9\ntrayButtons=sff\n");
        Settings g; g.autoHide = true; g.showWithStart = false; g.autostart = false; g.trayIcons = true; g.startMenuExtras = false; g.trayButtons = "sff";
        CheckSame(Dump(s), Dump(g), "flags: -1 on, ' 0' and 00 off, 1x on, empty keeps the default, 0.9 off");
        s = LoadFrom("edge=-1\niconSize=31.9\nmagnification=0.5\nglassStyle=3\nclockSize=2\niconSpacing=-1\niconGap=61\n"
                     "dockThickness=74\nedgeGap=-1\nedgeOffset=-101\nshowDelay=-0.1\nhideDelay=3.5\nrevealHotkey=-1\n"
                     "sensorSource=3\nadminWindows=-1\npinsVersion=1000001\ntrayButtons=\n");
        Settings lo; lo.edge = (Edge)0; lo.iconSize = 32; lo.magnification = 1; lo.glassStyle = 2; lo.clockSize = 1;
        lo.iconSpacing = 0; lo.iconGap = 60; lo.dockThickness = 75; lo.edgeGap = 0; lo.edgeOffset = -100; lo.showDelay = 0; lo.hideDelay = 3;
        lo.revealHotkey = 0; lo.sensorSource = 2; lo.adminWindows = 0; lo.pinsVersion = 1000000; lo.trayButtons = "";
        CheckSame(Dump(s), Dump(lo), "every range: one past either end is clamped to that end");
        s = LoadFrom("edge=3\niconSize=96\nmagnification=2.1\nglassStyle=0\nclockSize=1\niconSpacing=2\niconGap=0\n"
                     "dockThickness=150\nedgeGap=40\nedgeOffset=100\nshowDelay=2\nhideDelay=0\nrevealHotkey=3\n"
                     "sensorSource=0\nadminWindows=2\npinsVersion=1000000\nmagnification=2.1000001\ntrayButtons=x\n");
        Settings hi; hi.edge = Edge::Top; hi.iconSize = 96; hi.magnification = 2.1f; hi.glassStyle = 0; hi.clockSize = 1; hi.iconSpacing = 2;
        hi.iconGap = 0; hi.dockThickness = 150; hi.edgeGap = 40; hi.edgeOffset = 100; hi.showDelay = 2; hi.hideDelay = 0; hi.revealHotkey = 3;
        hi.sensorSource = 0; hi.adminWindows = 2; hi.pinsVersion = 1000000; hi.trayButtons = "x";
        CheckSame(Dump(s), Dump(hi), "every range: the ends themselves are kept");
        // lone CR line ends (no LF): one line, its first key takes the rest as its value
        s = LoadFrom("edge=1\riconSize=50\rtrayButtons=x\r");
        Settings cr; cr.edge = Edge::Left; cr.trayButtons = "desktop,theme";
        CheckSame(Dump(s), Dump(cr), "lone CR line ends: one line, edge=1 read, the rest lost, the list rebuilt from the switches");
        // two CRs before the LF: the text-mode read turns CR LF into LF, the code drops one more CR
        s = LoadFrom("hwinfoCpuLabel=CPU\r\r\ntrayButtons=x\r\r\n");
        Check(s.hwinfoCpuLabel == "CPU" && s.trayButtons == "x", "CR CR LF: both CRs are gone from the value");
        // Ctrl+Z (0x1A): the text-mode read ends the file there
        s = LoadFrom("edge=1\n\x1A\niconSize=50\ntrayButtons=x\n");
        Settings cz; cz.edge = Edge::Left; cz.trayButtons = "desktop,theme";
        CheckSame(Dump(s), Dump(cz), "a Ctrl+Z byte ends the file: nothing after it is read");
        // a NUL byte inside a value is kept in it
        const char nul[] = "trayButtons=a\0b\nedge=1\n";
        s = LoadFrom(std::string(nul, sizeof nul - 1));
        Check(s.trayButtons == std::string("a\0b", 3) && s.edge == Edge::Left, "a NUL byte inside a value is kept, the next line read");
        // the same key twice with a malformed one between: the last good one stays (here for a float and a text)
        s = LoadFrom("iconSize=50\niconSize=big\nhwinfoCpuLabel=A\nhwinfoCpuLabel=\ntrayButtons=x\n");
        Check(s.iconSize == 50 && s.hwinfoCpuLabel.empty(), "a malformed float keeps the last value; an empty text line empties it");
        // whitespace only, and a file of blank lines: nothing read, a fresh install's defaults
        CheckSame(Dump(LoadFrom(" \r\n\t\r\n\r\n\n")), Dump(Settings{}), "blank lines only: every default");
        CheckSame(Dump(LoadFrom("\xEF\xBB\xBF")), Dump(Settings{}), "a BOM alone: every default");
        CheckSame(Dump(LoadFrom("\xEF\xBB\xBF\r\n")), Dump(Settings{}), "a BOM and CRLF alone: every default");
    }
    // ---- Load fills in what the file has; what it does not have stays as it was in the object
    {
        Settings start = NonDefault(); start.cpuTemp = true; start.sourceFromCpuTemp = true;
        Settings s = LoadInto(start, "edge=1\n");
        Settings e = NonDefault(); e.edge = Edge::Left; e.cpuTemp = true;
        e.sensorSource = 1; e.sourceFromCpuTemp = false;   // no source line and no cpuTemp line in this file: HWiNFO, not marked
        e.trayButtons = "desktop,theme";   // rebuilt from the switches the object held (true, true, false)
        CheckSame(Dump(s), Dump(e), "Load into a used object: missing keys keep its values, the list and source are worked out again");
    }
}

// ---------------------------------------------------------------- more of Load and Save
// What the real SaveFileWhole leaves (its temporary file, its dock.log lines), three choices of Load, the key of every
// flag on its own, and the common.cpp helpers the OLED test replaces with copies of its own. Values are mod.35's.

// Right after SettingsChecks: this process's dock.log then holds the failed saves' lines and nothing else.
static void SaveFailureChecks() {
    const std::wstring tmp = Ini() + L".tmp";
    // a failed write takes its temporary file away: here held by another handle that allows deleting it
    HANDLE hold = CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Settings other; other.edge = Edge::Left; other.Save();
    const bool held = hold != INVALID_HANDLE_VALUE;
    if (held) CloseHandle(hold);
    Check(held && GetFileAttributesW(tmp.c_str()) == INVALID_FILE_ATTRIBUTES,
          "a save that cannot write its temporary file deletes it (gone once the other holder closes it)");
    DeleteFileW(tmp.c_str());
    // a failed swap (settings.ini read-only) leaves the temporary file there, holding the whole new text
    Settings{}.Save();
    SetFileAttributesW(Ini().c_str(), FILE_ATTRIBUTE_READONLY);
    Settings top; top.edge = Edge::Top; top.Save();
    const std::string left = ReadAll(tmp);
    SetFileAttributesW(Ini().c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(tmp.c_str());
    std::string want = ReadAll(Ini()); want.replace(0, 8, "edge=3\r\n");
    Check(left == want && ReadAll(Ini()).rfind("edge=0\r\n", 0) == 0, "a save whose swap fails leaves its temporary file, the whole new text in it, and the old file");
    // dock.log: the real SaveFileWhole's lines for the failed saves of SettingsChecks and the two above, label 'settings'
    std::string all = ReadAll(g_dir + L"\\dock.log");
    if (all.rfind("\xEF\xBB\xBF", 0) == 0) all.erase(0, 3);
    std::string texts; size_t p = 0;
    while (p < all.size()) {   // "HH:MM:SS.mmm " and the line's text, CRLF
        size_t e = all.find('\n', p); if (e == std::string::npos) e = all.size();
        std::string l = all.substr(p, e - p); if (!l.empty() && l.back() == '\r') l.pop_back();
        texts += (l.size() > 13 ? l.substr(13) : std::string("(short) ") + l) + "|"; p = e + 1;
    }
    CheckSame(texts,
              "settings: could not be written (error 5): settings.ini left as it was|"         // the temporary name is a folder
              "settings: could not replace settings.ini (error 5): left as it was|"           // settings.ini read-only
              "settings: could not be written (error 32): settings.ini left as it was|"        // the temporary file locked
              "settings: could not be written (error 32): settings.ini left as it was|"        // held, deletable
              "settings: could not replace settings.ini (error 5): left as it was|",          // read-only again
              "dock.log: the failed saves' lines, whole and in order (error numbers, the file's name, 'left as it was')");
}

static void MoreLoadChecks() {
    // a dock key whose value is malformed still makes the file one from before the button list
    Settings s = LoadFrom("edge=x\n");
    Settings e; e.trayButtons = "desktop,theme";
    CheckSame(Dump(s), Dump(e), "edge=x alone: a malformed dock key still counts, the list rebuilt as desktop,theme");
    // a text key other than trayButtons is not the button list
    s = LoadFrom("hwinfoCpuLabel=X\n");
    e.hwinfoCpuLabel = "X";
    CheckSame(Dump(s), Dump(e), "hwinfoCpuLabel=X alone: taken, and it is not the button list (rebuilt as desktop,theme)");
    // numbers beyond float's range are malformed (std::stof throws), not clamped to the range's end
    s = LoadFrom("iconSize=1e40\nshowDelay=1e-50\nhideDelay=1e39\nmagnification=-1e40\ntrayButtons=x\n");
    Settings f; f.trayButtons = "x";
    CheckSame(Dump(s), Dump(f), "1e40, 1e-50, 1e39 and -1e40: beyond float, ignored (no clamp), the defaults stay");
    s = LoadFrom("iconSize=1e38\nshowDelay=1e-40\ntrayButtons=x\n");
    f.iconSize = 96; f.showDelay = 1e-40f;
    CheckSame(Dump(s), Dump(f), "1e38 is clamped to 96; 1e-40 (a denormal) is taken as it is");
}

// One flag at a time, by a table of this test's own: each key reaches its own member, each member its own line.
static void OneFlagChecks() {
    struct One { const char* key; bool Settings::* m; };
    static const One ones[] = {
        { "oledOneMonitor", &Settings::oledOneMonitor }, { "trayIcons", &Settings::trayIcons }, { "fluentFlyout", &Settings::fluentFlyout },
        { "autoHide", &Settings::autoHide }, { "secondaryAlwaysShown", &Settings::secondaryAlwaysShown }, { "showWithStart", &Settings::showWithStart },
        { "autostart", &Settings::autostart }, { "startMenuExtras", &Settings::startMenuExtras }, { "shiftRunsAsAdmin", &Settings::shiftRunsAsAdmin },
        { "taskbarAppButtons", &Settings::taskbarAppButtons }, { "showCpuTemp", &Settings::showCpuTemp }, { "showGpu", &Settings::showGpu },
        { "showVram", &Settings::showVram }, { "showVramTemp", &Settings::showVramTemp }, { "showRamTemp", &Settings::showRamTemp },
        { "showSsd", &Settings::showSsd }, { "showCpuPower", &Settings::showCpuPower }, { "showGpuPower", &Settings::showGpuPower },
    };
    const std::string def = SavedText(Settings{});
    bool readOk = true, writeOk = true;
    for (const One& o : ones) {
        const bool was = Settings{}.*o.m;
        const std::string from = std::string(o.key) + "=" + (was ? "1" : "0") + "\r\n", to = std::string(o.key) + "=" + (was ? "0" : "1") + "\r\n";
        Settings e; e.*o.m = !was;
        if (Dump(LoadFrom("trayButtons=desktop,theme,sff\n" + to)) != Dump(e)) { readOk = false; printf("  read: %s\n", o.key); }
        std::string want = def;
        const size_t at = want.find("\n" + from);
        if (at == std::string::npos) { writeOk = false; printf("  no line: %s\n", o.key); continue; }
        want.replace(at + 1, from.size(), to);
        if (SavedText(e) != want) { writeOk = false; printf("  write: %s\n", o.key); }
    }
    Check(readOk, "each of the 18 flags read alone changes its own member and no other");
    Check(writeOk, "each of the 18 flags saved alone changes its own line and no other");
}

// common.cpp's FromUtf8 and ToLower (preview_oled_test pins oled.ini's reading on copies of its own)
static void HelperChecks() {
    const std::wstring w = FromUtf8("M\xFF.jpg");
    Check(w.size() == 6 && w[0] == L'M' && w[1] == 0xFFFD && w.substr(2) == L".jpg", "FromUtf8: a byte that is not UTF-8 becomes U+FFFD, the rest kept");
    Check(FromUtf8("\xC3") == std::wstring(1, (wchar_t)0xFFFD) && FromUtf8("").empty(), "FromUtf8: a cut-off sequence is one U+FFFD; empty stays empty");
    Check(ToLower(L"C:\\\u00C9T\u00C9\\OLED_BLACK.BMP") == L"c:\\\u00E9t\u00E9\\oled_black.bmp", "ToLower lowers letters beyond ASCII too (E acute to e acute)");
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
    SaveFailureChecks();
    GoldenChecks();
    MoreLoadChecks();
    OneFlagChecks();
    HelperChecks();
    LogChecks(argv[0]);
    DeleteFileW(Ini().c_str());
    return Summary("dock_render_settings");
}
