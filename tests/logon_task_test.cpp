// The logon task's XML (src/logon_task.h, SetAutostart in src/main_autostart.cpp): the whole text for an account and
// the dock's path, the escaping of &, < and > in both, CRLF line ends, the account in the trigger and the principal.
// tests\main_loop_test.cpp checks the file SetAutostart writes from it (BOM, UTF-16LE bytes) and the schtasks line.
// No Windows headers: builds with any C++20 compiler.
//   g++ -std=c++20 tests/logon_task_test.cpp -o logon_task_test
#include "../src/logon_task.h"
#include "portable_check.h"
#include <cstdio>
#include <string>

static size_t Count(const std::wstring& s, const std::wstring& part) {
    size_t n = 0;
    for (size_t at = s.find(part); at != std::wstring::npos; at = s.find(part, at + part.size())) ++n;
    return n;
}

int main() {
    const std::wstring exe = L"C:\\Users\\User\\AppData\\Local\\Programs\\LiquidDock\\LiquidDock.exe";
    const std::wstring xml = LogonTaskXml(L"User", L"DESKTOP-TEST01", exe);
    const std::wstring expected =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Description>Starts Liquid Dock at logon</Description></RegistrationInfo>\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>DESKTOP-TEST01\\User</UserId></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>DESKTOP-TEST01\\User</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority><Enabled>true</Enabled></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>\"C:\\Users\\User\\AppData\\Local\\Programs\\LiquidDock\\LiquidDock.exe\"</Command></Exec></Actions>\r\n"
        L"</Task>\r\n";
    Check(xml == expected, "the task's whole text for DESKTOP-TEST01\\User and the installed dock's path");
    Check(xml.size() == expected.size() && xml.size() == 933, "...933 characters (1,866 bytes in the UTF-16 file, before its BOM)");
    Check(Count(xml, L"\r\n") == 8 && Count(xml, L"\n") == 8 && Count(xml, L"\r") == 8, "...eight lines, each ending in CRLF (no bare LF or CR)");
    Check(Count(xml, L"<UserId>DESKTOP-TEST01\\User</UserId>") == 2, "...the account in the logon trigger and as the principal");
    Check(xml.find(L"<Command>\"" + exe + L"\"</Command>") != std::wstring::npos, "...the exe quoted in the command");
    Check(xml.rfind(L"</Task>\r\n") == xml.size() - 9, "...ending in </Task> and its CRLF");

    const std::wstring odd = LogonTaskXml(L"a&b", L"<d>", L"C:\\x&y\\<z>.exe");
    Check(Count(odd, L"<UserId>&lt;d&gt;\\a&amp;b</UserId>") == 2, "&, < and > in the account: escaped as &amp; &lt; &gt; (domain before the backslash)");
    Check(odd.find(L"<Command>\"C:\\x&amp;y\\&lt;z&gt;.exe\"</Command>") != std::wstring::npos, "...and in the exe's path");
    Check(Count(odd, L"&amp;") == 3 && Count(odd, L"&lt;") == 3 && Count(odd, L"&gt;") == 3, "...each one, nothing else (the markup's own < > not counted twice)");
    const std::wstring korean = LogonTaskXml(L"사용자", L"집", L"D:\\독 'a'\\LiquidDock.exe");
    Check(Count(korean, L"<UserId>집\\사용자</UserId>") == 2 && korean.find(L"<Command>\"D:\\독 'a'\\LiquidDock.exe\"</Command>") != std::wstring::npos,
          "other characters (Korean, spaces, apostrophes) as they are");
    const std::wstring none = LogonTaskXml(L"", L"", L"");
    Check(Count(none, L"<UserId>\\</UserId>") == 2 && none.find(L"<Command>\"\"</Command>") != std::wstring::npos,
          "empty account and path (the environment unread): a lone backslash and an empty quoted command");

    Check(LogonTaskEscape(L"").empty() && LogonTaskEscape(L"plain \"text\"") == L"plain \"text\"", "the escape: empty stays empty, quotes are left alone");
    Check(LogonTaskEscape(L"&&<<>>") == L"&amp;&amp;&lt;&lt;&gt;&gt;", "...every &, < and >, one after another");
    Check(LogonTaskEscape(L"&amp;") == L"&amp;amp;", "...an & already escaped is escaped again (the text is taken as plain)");
    return Summary("logon_task_test");
}
