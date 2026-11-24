#pragma once
// The per-user logon task that starts the dock (SetAutostart: schtasks /Create /XML from a file in %TEMP%, written as
// UTF-16 with a byte order mark), its XML kept free of Windows headers so it can be tested anywhere
// (tests/logon_task_test.cpp, run by run_regressions.bat and tests/run_portable.sh). The bytes are the task's
// definition: tests\main_loop_test.cpp checks the whole file SetAutostart writes.
#include <string>

// &, < and > as XML text (the account name and the exe's path go into it)
inline std::wstring LogonTaskEscape(std::wstring s) {
    std::wstring o;
    for (wchar_t c : s) { if (c == L'&') o += L"&amp;"; else if (c == L'<') o += L"&lt;"; else if (c == L'>') o += L"&gt;"; else o += c; }
    return o;
}

// The task for domain\user (%USERDOMAIN%, %USERNAME%: at that account's logon, as that account, not elevated) that runs
// exe (the dock's own path, quoted): one copy at a time, on battery too, no time limit
inline std::wstring LogonTaskXml(const std::wstring& user, const std::wstring& domain, const std::wstring& exe) {
    std::wstring who = LogonTaskEscape(domain + L"\\" + user);
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Description>Starts Liquid Dock at logon</Description></RegistrationInfo>\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + who + L"</UserId></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>" + who + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority><Enabled>true</Enabled></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>\"" + LogonTaskEscape(exe) + L"\"</Command></Exec></Actions>\r\n"
        L"</Task>\r\n";
    return xml;
}
