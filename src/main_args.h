#pragma once
// LiquidDock.exe's command line (main.cpp wWinMain): what a run is asked to do. Kept free of Windows headers so it
// can be tested anywhere (tests/main_args_test.cpp, run by run_regressions.bat and tests/run_portable.sh).
// A flag is a plain substring of the whole line, as it always was: "--quit" anywhere on it, "--restored" counts as
// --restore, the order of the flags does not matter (the shortcuts, Setup and tools\demo_record.ps1 pass one each).
// main.cpp acts on them in its own order: the watchdog copy, a developer flag, --settings, --restore/--quit, the hand-over
// of an elevated start, then the dock's own flags.
#include <cstddef>
#include <cwchar>
#include <string>

namespace mainargs {

// "--watchdog <pid> --ready <event name>" (main_instance.cpp StartWatchdog starts it): the copy that waits for the dock
// to exit. main.cpp looks for it first, on the raw line, before DPI awareness and COM are set up (the copy needs
// neither). pid: the text after "--watchdog " (main.cpp reads its number there); ready: the rest of the line after
// " --ready ", to its end (spaces and later flags too), nullptr without one. false: not the watchdog copy (no line,
// or "--watchdog" without the space after it).
struct WatchdogArgs { const wchar_t* pid = nullptr; const wchar_t* ready = nullptr; };
inline bool FindWatchdog(const wchar_t* cmd, WatchdogArgs& out) {
    if (!cmd) return false;
    const wchar_t* watchdog = std::wcsstr(cmd, L"--watchdog ");
    if (!watchdog) return false;
    const wchar_t* ready = std::wcsstr(cmd, L" --ready ");
    out.pid = watchdog + 11;
    out.ready = ready ? ready + 9 : nullptr;
    return true;
}

// The flags of the other runs and of the dock's own start
struct LaunchArgs {
    bool settings = false;    // --settings: a running dock opens its settings window (no dock: this run starts one, no window)
    bool restore = false;     // --restore (the restore shortcut, the uninstaller): the dock ended, the taskbar back, OLED undone
    bool quit = false;        // --quit: the dock ended, the taskbar back
    bool keepAdmin = false;   // --keep-admin: a start as administrator stays elevated
    bool fromAdmin = false;   // --from-admin: the copy a start as administrator handed to the user (not handed over again)
    bool demo = false;        // --demo (g_demoMode)
    bool welcome = false;     // --welcome (Setup, right after installing): the dock shown for a few seconds
    bool profile = false;     // --profile: the main loop's time per part in dock.log every 10 s
};
inline LaunchArgs ParseLaunchArgs(const std::wstring& args) {
    auto has = [&args](const wchar_t* flag) { return args.find(flag) != std::wstring::npos; };
    LaunchArgs a;
    a.settings = has(L"--settings");
    a.restore = has(L"--restore");
    a.quit = has(L"--quit");
    a.keepAdmin = has(L"--keep-admin");
    a.fromAdmin = has(L"--from-admin");
    a.demo = has(L"--demo");
    a.welcome = has(L"--welcome");
    a.profile = has(L"--profile");
    return a;
}

// A developer build's flags (LD_DEV: main.cpp's table of flag -> message): the first entry of the table whose flag is
// on the line - the table's order, not the line's. nullptr: none of them.
template <class Entry, std::size_t N> const Entry* FirstFlag(const std::wstring& args, const Entry (&table)[N]) {
    for (const Entry& e : table)
        if (args.find(e.flag) != std::wstring::npos) return &e;
    return nullptr;
}

}  // namespace mainargs
