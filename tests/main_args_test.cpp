// LiquidDock.exe's command line (src/main_args.h, main.cpp wWinMain): the watchdog copy's "--watchdog <pid> --ready <name>",
// the flags of the other runs and of the dock's start, and the first-match rule of a developer build's table. A flag
// is a plain substring of the whole line (as main.cpp always read it); the ready name runs to the end of the line.
// tests\main_loop_test.cpp checks the same through wWinMain (what each run then does); this one checks the reading alone.
// No Windows headers: builds with any C++20 compiler.
//   g++ -std=c++20 tests/main_args_test.cpp -o main_args_test
#include "../src/main_args.h"
#include "portable_check.h"
#include <cstdio>
#include <cwchar>
#include <string>

using namespace mainargs;

static bool Same(const wchar_t* a, const wchar_t* b) { return a && b && std::wcscmp(a, b) == 0; }

static void WatchdogTests() {
    WatchdogArgs w;
    Check(!FindWatchdog(nullptr, w) && !w.pid && !w.ready, "no command line: not the watchdog copy, nothing set");
    Check(!FindWatchdog(L"", w), "an empty line: not the watchdog copy");
    Check(!FindWatchdog(L"--watchdog", w) && !FindWatchdog(L"--watchdog\t12", w), "\"--watchdog\" without the space after it: not the watchdog copy (the dock starts)");
    Check(!FindWatchdog(L"--welcome --from-admin", w), "the dock's own flags: not the watchdog copy");

    // as StartWatchdog writes it (main_instance.cpp): "<exe>" --watchdog <pid> --ready <name>; wWinMain gets the part after the exe
    const wchar_t* line = L"--watchdog 4321 --ready Local\\LiquidDock.WatchdogReady.{0F0E0D0C-0B0A-0908-0706-050403020100}";
    w = {};
    Check(FindWatchdog(line, w), "the watchdog copy's line is found");
    Check(w.pid == line + 11 && Same(w.pid, L"4321 --ready Local\\LiquidDock.WatchdogReady.{0F0E0D0C-0B0A-0908-0706-050403020100}"),
          "...pid: the text right after \"--watchdog \" (main.cpp reads its number there)");
    Check(w.ready == line + 24 && Same(w.ready, L"Local\\LiquidDock.WatchdogReady.{0F0E0D0C-0B0A-0908-0706-050403020100}"),
          "...ready: the event name after \" --ready \", pointing into the line itself");

    const wchar_t* spaced = L"--watchdog 9 --ready a name with spaces --quit ";
    w = {};
    Check(FindWatchdog(spaced, w) && Same(w.ready, L"a name with spaces --quit "), "the ready name runs to the end of the line: spaces, later flags and a trailing space too");
    w = {};
    Check(FindWatchdog(L"--watchdog 17", w) && Same(w.pid, L"17") && !w.ready, "no \" --ready \": ready is nullptr (an old watchdog line: it just waits)");
    w = {};
    Check(FindWatchdog(L"--watchdog 17 --ready", w) && !w.ready, "\"--ready\" at the end with nothing after its space: nullptr");
    w = {};
    Check(FindWatchdog(L"--ready x --watchdog 17", w) && !w.ready && Same(w.pid, L"17"), "\"--ready\" at the start of the line (no space before it): not seen");
    const wchar_t* before = L"x --ready r --watchdog 5";
    w = {};
    Check(FindWatchdog(before, w) && Same(w.ready, L"r --watchdog 5") && Same(w.pid, L"5"), "\" --ready \" before \"--watchdog \" still counts, from the whole line");
    const wchar_t* late = L"--quit --settings --watchdog 33";
    w = {};
    Check(FindWatchdog(late, w) && w.pid == late + 29, "\"--watchdog \" anywhere on the line wins over every other flag (checked first)");
    const wchar_t* twice = L"--watchdog 1 --watchdog 2";
    w = {};
    Check(FindWatchdog(twice, w) && w.pid == twice + 11, "two of them: the first one's number");
    w = {};
    Check(FindWatchdog(L"--watchdog x", w) && Same(w.pid, L"x"), "a pid that is no number is passed on as it is (main.cpp's _wtoi makes it 0)");
}

static bool Only(const LaunchArgs& a, bool LaunchArgs::*flag) {
    bool LaunchArgs::*all[] = { &LaunchArgs::settings, &LaunchArgs::restore, &LaunchArgs::quit, &LaunchArgs::keepAdmin,
                                &LaunchArgs::fromAdmin, &LaunchArgs::demo, &LaunchArgs::welcome, &LaunchArgs::profile };
    for (auto f : all)
        if ((a.*f) != (f == flag)) return false;
    return true;
}

static void LaunchTests() {
    Check(Only(ParseLaunchArgs(L""), nullptr), "an empty line: no flag");
    Check(Only(ParseLaunchArgs(L"--unknown -x /quit"), nullptr), "unknown arguments and other spellings: no flag");
    Check(Only(ParseLaunchArgs(L"--settings"), &LaunchArgs::settings), "--settings alone");
    Check(Only(ParseLaunchArgs(L"--restore"), &LaunchArgs::restore), "--restore alone (the restore shortcut, the uninstaller)");
    Check(Only(ParseLaunchArgs(L"--quit"), &LaunchArgs::quit), "--quit alone");
    Check(Only(ParseLaunchArgs(L"--keep-admin"), &LaunchArgs::keepAdmin), "--keep-admin alone");
    Check(Only(ParseLaunchArgs(L"--from-admin"), &LaunchArgs::fromAdmin), "--from-admin alone");
    Check(Only(ParseLaunchArgs(L"--demo"), &LaunchArgs::demo), "--demo alone");
    Check(Only(ParseLaunchArgs(L"--welcome"), &LaunchArgs::welcome), "--welcome alone (Setup)");
    Check(Only(ParseLaunchArgs(L"--profile"), &LaunchArgs::profile), "--profile alone");

    Check(Only(ParseLaunchArgs(L"--restored"), &LaunchArgs::restore), "a substring: \"--restored\" is --restore");
    Check(Only(ParseLaunchArgs(L"x--settingsX"), &LaunchArgs::settings), "...\"x--settingsX\" is --settings");
    Check(Only(ParseLaunchArgs(L"--QUIT --Settings"), nullptr), "...and case counts: \"--QUIT\", \"--Settings\" are none");
    const LaunchArgs both = ParseLaunchArgs(L"--quit --settings");
    Check(both.quit && both.settings && !both.restore, "two flags: both seen (main.cpp then acts on --settings first)");
    const LaunchArgs handed = ParseLaunchArgs(L"--welcome --profile --from-admin");
    Check(handed.welcome && handed.profile && handed.fromAdmin && !handed.keepAdmin, "the hand-over's line (the flags + \" --from-admin\"): every flag kept");
    const LaunchArgs ready = ParseLaunchArgs(L"--watchdog 5 --ready Local\\x --quit");
    Check(ready.quit && !ready.restore, "the flags are read from the whole line (main.cpp looks for the watchdog copy before)");
}

struct Entry { const wchar_t* flag; unsigned msg; };

static void DevTableTests() {
    static constexpr Entry table[] = { { L"--rec-start", 17 }, { L"--rec-stop", 18 }, { L"--debug-show", 10 }, { L"--debug-hide", 11 } };
    Check(FirstFlag(std::wstring(L""), table) == nullptr, "a developer table: no flag on the line - none");
    Check(FirstFlag(std::wstring(L"--settings --welcome"), table) == nullptr, "...other flags only - none");
    Check(FirstFlag(std::wstring(L"--debug-show"), table) == &table[2], "...one flag: its entry (the table's own element)");
    Check(FirstFlag(std::wstring(L"--debug-hide --rec-stop"), table) == &table[1], "...two flags: the one earlier in the table, not on the line");
    Check(FirstFlag(std::wstring(L"x--rec-startx"), table) == &table[0], "...a substring counts");
    static constexpr Entry prefix[] = { { L"--rec", 1 }, { L"--rec-full", 2 } };
    Check(FirstFlag(std::wstring(L"--rec-full"), prefix) == &prefix[0], "...an entry that is a prefix of a later one wins when earlier in the table");
}

int main() {
    WatchdogTests();
    LaunchTests();
    DevTableTests();
    return Summary("main_args_test");
}
