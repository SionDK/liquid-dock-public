// The admin window service's shared rules (src/admin_protocol.h): the messages, which windows the
// elevated helper may touch, who may talk to whom, when the dock needs the helper, helper versions.
// No Windows headers: builds with any C++20 compiler.
//   cl /std:c++20 /EHsc /utf-8 tests\admin_protocol_test.cpp
//   g++ -std=c++20 tests/admin_protocol_test.cpp -o admin_protocol_test
#include "../src/admin_protocol.h"
#include "portable_check.h"

using namespace adminhelper;

int main() {
    // ---- requests
    Request in = MakeRequest(Command::Activate, 0x1234), out{};
    Check(ParseRequest(&in, sizeof(in), out) && out.command == (uint32_t)Command::Activate && out.window == 0x1234, "a request survives the round trip");
    Check(!ParseRequest(&in, sizeof(in) - 1, out) && !ParseRequest(&in, sizeof(in) + 1, out), "exactly one request: no shorter or longer message");
    Check(!ParseRequest(nullptr, sizeof(in), out), "no data");
    Request bad = in;
    bad.magic ^= 1;
    Check(!ParseRequest(&bad, sizeof(bad), out), "another program's message (magic)");
    bad = in;
    bad.version = 0;
    Check(!ParseRequest(&bad, sizeof(bad), out), "version 0 is not this protocol");
    bad = in;
    bad.version = 7;
    Check(ParseRequest(&bad, sizeof(bad), out), "a newer dock's request is served in the commands this helper knows");
    bad = in;
    bad.command = 4;
    Check(!ParseRequest(&bad, sizeof(bad), out), "an unknown command");
    for (Command c : { Command::Activate, Command::Minimize, Command::Close }) {
        Request r = MakeRequest(c, 0);
        Check(!ParseRequest(&r, sizeof(r), out), "every window command needs a window");
    }
    Request hello = MakeRequest(Command::Hello, 0);
    Check(ParseRequest(&hello, sizeof(hello), out), "Hello needs none");
    bad = MakeRequest(Command::Close, 0x42);
    Check(ParseRequest(&bad, sizeof(bad), out) && out.command == (uint32_t)Command::Close, "the last known command (Close)");
    bad.version = 2;
    bad.command = 9;
    Check(!ParseRequest(&bad, sizeof(bad), out), "a newer dock's command this helper does not know: refused, not guessed");
    bad = in;
    bad.command = 0xFFFFFFFFu;
    Check(!ParseRequest(&bad, sizeof(bad), out), "a command number far out of range");
    bad = in;
    bad.reserved = 0xDEADBEEFu;
    Check(ParseRequest(&bad, sizeof(bad), out), "the reserved field is not read");
    unsigned char two[sizeof(Request) * 2];
    std::memcpy(two, &in, sizeof(in));
    std::memcpy(two + sizeof(in), &in, sizeof(in));
    Check(!ParseRequest(two, sizeof(two), out), "two requests in one message: refused (one request per connection)");

    // ---- replies
    Reply rep = MakeReply(Result::NotAllowed), got{};
    Check(ParseReply(&rep, sizeof(rep), got) && got.result == (uint32_t)Result::NotAllowed, "a reply survives the round trip");
    Check(!ParseReply(&rep, sizeof(rep) - 4, got), "a reply has one size");
    Reply odd = MakeReply(Result::NotRunning);
    Check(!ParseReply(&odd, sizeof(odd), got), "the helper never answers with a dock-side result");
    odd = rep;
    odd.magic = 0;
    Check(!ParseReply(&odd, sizeof(odd), got), "not a reply of this protocol");
    odd = MakeReply(Result::Failed);
    Check(ParseReply(&odd, sizeof(odd), got) && got.result == (uint32_t)Result::Failed, "the last helper-side result (refused by Windows)");
    odd.version = 0;
    Check(!ParseReply(&odd, sizeof(odd), got), "a reply of version 0");
    Check(!ParseReply(nullptr, sizeof(odd), got), "a reply without data");
    Check(std::wstring(ResultName((Result)99)) == L"?", "an unknown result is named '?' in the log");
    bool named = true;
    for (uint32_t r = 0; r <= (uint32_t)Result::NoAnswer; ++r) named = named && std::wstring(ResultName((Result)r)) != L"?";
    Check(named, "every result has a name for the log");

    // ---- which windows: the ones with a taskbar button
    WindowFacts w;
    w.exists = true; w.visible = true; w.topLevel = true; w.className = L"Qt5152QWindowIcon";
    Check(Eligible(w) == Result::Ok, "an app's main window (OBS: a Qt window)");
    WindowFacts gone;
    Check(Eligible(gone) == Result::NoWindow, "no such window");
    WindowFacts f = w; f.visible = false;
    Check(Eligible(f) == Result::NotAllowed, "a hidden window (minimized to the tray, say) has no button");
    f = w; f.topLevel = false;
    Check(Eligible(f) == Result::NotAllowed, "a child window");
    f = w; f.owned = true;
    Check(Eligible(f) == Result::NotAllowed, "an owned window (a dialog)");
    f.exStyle = kExAppWindow;
    Check(Eligible(f) == Result::Ok, "an owned window marked as an app window has a button");
    f = w; f.exStyle = kExToolWindow;
    Check(Eligible(f) == Result::NotAllowed, "a tool window");
    f.exStyle = kExToolWindow | kExAppWindow;
    Check(Eligible(f) == Result::Ok, "a tool window marked as an app window");
    f = w; f.exStyle = kExNoActivate;
    Check(Eligible(f) == Result::NotAllowed, "a no-activate window");
    bool shell = true;
    for (const wchar_t* c : { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" }) {
        f = w; f.className = c; f.exStyle = kExAppWindow;
        shell = shell && Eligible(f) == Result::NotAllowed;
    }
    Check(shell, "never the desktop or a taskbar, whatever their styles");
    f = w; f.className = L"Progman2";
    Check(Eligible(f) == Result::Ok, "a class only named like the desktop's is an app's");
    f = w; f.visible = false; f.exStyle = kExAppWindow;
    Check(Eligible(f) == Result::NotAllowed, "a hidden app window has no button either");
    f = w; f.topLevel = false; f.exStyle = kExAppWindow;
    Check(Eligible(f) == Result::NotAllowed, "nor a child window marked as an app window");
    f = w; f.owned = true; f.exStyle = kExNoActivate | kExAppWindow;
    Check(Eligible(f) == Result::Ok, "an owned no-activate window marked as an app window has a button");
    gone.visible = true; gone.topLevel = true; gone.exStyle = kExAppWindow;
    Check(Eligible(gone) == Result::NoWindow, "a window gone is 'no window' whatever was read before");

    // ---- who talks to whom
    Check(PipeName(3) == L"\\\\.\\pipe\\LiquidDock.AdminHelper.3", "one pipe per session");
    Check(PipeName(1, L"\\\\.\\pipe\\T.") == L"\\\\.\\pipe\\T.1", "another prefix (tests)");
    Check(FileOf(L"C:\\a\\b\\LiquidDock.exe") == L"LiquidDock.exe" && FileOf(L"x/y.exe") == L"y.exe" && FileOf(L"z.exe") == L"z.exe", "file name of a path");
    Check(ClientAllowed(L"C:\\Users\\USER\\AppData\\Local\\Programs\\LiquidDock\\liquiddock.EXE", L"LiquidDock.exe"), "the dock, in any letter case");
    Check(!ClientAllowed(L"C:\\x\\LiquidDock.exe.bak", L"LiquidDock.exe") && !ClientAllowed(L"C:\\x\\Other.exe", L"LiquidDock.exe"), "other programs are refused");
    Check(!ClientAllowed(L"", L"LiquidDock.exe"), "a program whose file could not be read is refused");
    const std::wstring installed = L"C:\\Program Files\\LiquidDock\\LiquidDockSensors.exe";
    Check(ServerTrusted(L"c:\\program files\\liquiddock\\LIQUIDDOCKSENSORS.exe", installed), "the installed helper, in any letter case");
    Check(!ServerTrusted(L"C:\\Users\\USER\\AppData\\Local\\Programs\\LiquidDock\\LiquidDockSensors.exe", installed),
          "the same file name in the user's (writable) folder is not trusted");
    Check(!ServerTrusted(L"", installed) && !ServerTrusted(installed, L""), "nothing to compare: not trusted");
    Check(!ServerTrusted(L"C:\\Program Files\\LiquidDock\\LiquidDockSensors.exe.bak", installed) &&
          !ServerTrusted(L"D:\\Program Files\\LiquidDock\\LiquidDockSensors.exe", installed), "another file or another drive: not trusted");
    Check(!ClientAllowed(L"C:\\x\\LiquidDock.exe\\", L"LiquidDock.exe") && !ClientAllowed(L"C:\\x\\LiquidDock.exe:alt", L"LiquidDock.exe"),
          "a folder named like the dock, or a stream of its file: refused");
    Check(ClientAllowed(L"LiquidDock.exe", L"LiquidDock.exe"), "a bare file name (no folder)");
    Check(PipeName(0) == L"\\\\.\\pipe\\LiquidDock.AdminHelper.0" && PipeName(4294967295u) == L"\\\\.\\pipe\\LiquidDock.AdminHelper.4294967295",
          "session 0 and the largest session number");

    // ---- when the dock needs the helper (integrity levels)
    Check(NeedsHelper(0x3000, 0x2000, false), "high (run as administrator) above medium (the dock)");
    Check(NeedsHelper(0x4000, 0x2000, false), "system above medium");
    Check(!NeedsHelper(0x2000, 0x2000, false) && !NeedsHelper(0x1000, 0x2000, false), "same or lower level: the dock itself");
    Check(!NeedsHelper(0x3000, 0x3000, false), "a dock kept elevated (--keep-admin) needs no helper");
    Check(NeedsHelper(0, 0x2000, true), "level unknown (its token could not be read): ask the helper");

    // ---- installed helper versions
    Check(HelperCurrent(PackVersion(0x00000002, 0x0000000A)), "0.2.0.10 has the window service");
    Check(!HelperCurrent(PackVersion(0x00000002, 0x00000009)), "0.2.0.9 has not: updated first");
    Check(HelperCurrent(PackVersion(0x00000002, 0x0000000B)) && HelperCurrent(PackVersion(0x00000003, 0)) &&
          HelperCurrent(PackVersion(0x00010000, 0)), "later versions (0.2.0.11, 0.3.0.0, 1.0.0.0)");
    Check(!HelperCurrent(0), "no version resource (every helper before mod.10)");
    Check(PackVersion(0x00000002, 0x00000016) == ((2ull << 32) | 22ull) && HelperCurrent(PackVersion(0x00000002, 0x00000016)),
          "0.2.0.22 (mod.22, LD_VERSION_BIN 0,2,0,22) packs as major.minor.build.revision and is current");
    Check(!HelperCurrent(PackVersion(0x00000001, 0xFFFFFFFFu)), "0.1.65535.65535 is older than the service");

    // ---- when the installed helper is (re)installed from the shipped copy (byte for byte)
    const char shipped[] = "MZ helper mod.33", same[] = "MZ helper mod.33", rebuilt[] = "MZ helper mod.3X", older[] = "MZ helper";
    Check(!HelperNeedsInstall(true, same, sizeof(same), true, shipped, sizeof(shipped)), "the same bytes: nothing installed (no UAC prompt)");
    Check(HelperNeedsInstall(true, rebuilt, sizeof(rebuilt), true, shipped, sizeof(shipped)), "the same size, other bytes (a rebuild): installed");
    Check(HelperNeedsInstall(true, older, sizeof(older), true, shipped, sizeof(shipped)), "another size: installed");
    Check(HelperNeedsInstall(false, nullptr, 0, true, shipped, sizeof(shipped)), "none installed (or unreadable): installed");
    Check(!HelperNeedsInstall(true, older, sizeof(older), false, nullptr, 0), "no shipped copy to install from: the installed one stays");
    Check(HelperNeedsInstall(false, nullptr, 0, false, nullptr, 0), "neither: tried (the install itself then fails, as before)");
    Check(!HelperNeedsInstall(true, nullptr, 0, true, nullptr, 0), "two empty files are the same");
    return Summary("admin_protocol_test");
}
