#pragma once
// Whether a process runs elevated (as administrator), from its token. Inline in a header of its own: the
// tray host, FluentFlyout and main ask it, and the tests link no common.cpp. Being inline, it calls whatever
// OpenProcessToken and GetTokenInformation name where it is first included: a test that fakes those calls defines
// its fakes before that. A test that fakes the answer instead keeps this one real and unused, and renames the
// callers' ProcessElevated to its stub after this include (fluent_lifecycle_test).
#include <windows.h>

// process: a handle with PROCESS_QUERY_LIMITED_INFORMATION, or GetCurrentProcess(). False when its token
// cannot be read.
inline bool TokenElevated(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

// whether the dock runs as administrator
inline bool ProcessElevated() { return TokenElevated(GetCurrentProcess()); }
