// Small stand-ins for common.cpp and small helpers that several tests share.
// Each piece is chosen by a macro defined just before the include, and the header is included
// at the place of the piece it replaces, so a stub keeps its place against the test's src\ includes:
//   #define TEST_SUPPORT_QPC_NOW
//   #include "test_support.h"
// No #pragma once and no #include of its own: the macro is #undef'd once its piece is written, so the header
// can be included again further down for another piece, also inside a namespace. Before it the test includes
// <windows.h> (a src\ header does) and <string>. Not for tests that link common.cpp (they have the real ones).
// Kept in the tests, not here: Log (each test does something else with a line: collects it, counts kinds of
// lines, slows it down, prints it or not) and clocks that start at the test's start, stand still or are skewed.
//
//   TEST_SUPPORT_QPC_NOW     double NowSeconds(): QueryPerformanceCounter in seconds, as common.cpp's
//   TEST_SUPPORT_FROM_UTF8   std::wstring FromUtf8(const std::string&): common.h's, UTF-8 to UTF-16
//   TEST_SUPPORT_U8          std::string U8(const std::wstring&): UTF-16 to UTF-8 for printf (the live tests of
//                            the taskbar's app buttons)
//   TEST_SUPPORT_WAIT_TICKS  template WaitFor(ok, ms = 3000): ok() every 20 ms until it holds (true) or ms have
//                            passed on GetTickCount64 (false) - the admin helper's pipe tests; admin_policy_test
//                            looks every 10 ms and keeps its own
//   TEST_SUPPORT_FOREGROUND_RIGHT_TEXT  const wchar_t* ForegroundRightText(ForegroundRight): shell_front.cpp's words
//                            (system_state_test checks the real one) - the start button's tests (its dock.log lines)

#ifdef TEST_SUPPORT_QPC_NOW
#undef TEST_SUPPORT_QPC_NOW
double NowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}
#endif

#ifdef TEST_SUPPORT_FROM_UTF8
#undef TEST_SUPPORT_FROM_UTF8
std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
#endif

#ifdef TEST_SUPPORT_U8
#undef TEST_SUPPORT_U8
std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
#endif

#ifdef TEST_SUPPORT_FOREGROUND_RIGHT_TEXT
#undef TEST_SUPPORT_FOREGROUND_RIGHT_TEXT
const wchar_t* ForegroundRightText(ForegroundRight r) {
    switch (r) {
    case ForegroundRight::Had: return L"had it";
    case ForegroundRight::Taken: return L"taken with an unassigned key";
    case ForegroundRight::Refused: return L"refused even after an unassigned key";
    case ForegroundRight::NotInserted: return L"not taken: the key was not inserted";
    }
    return L"?";
}
#endif

#ifdef TEST_SUPPORT_WAIT_TICKS
#undef TEST_SUPPORT_WAIT_TICKS
template <class F> bool WaitFor(F&& ok, DWORD ms = 3000) {
    ULONGLONG until = GetTickCount64() + ms;
    while (!ok()) {
        if (GetTickCount64() > until) return false;
        Sleep(20);
    }
    return true;
}
#endif
