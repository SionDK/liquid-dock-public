#pragma once
// A minimal check harness for tests that need no Windows headers (they build with any C++20
// compiler: cl, g++, clang++). Same output style as the other tests: one PASS/FAIL line per check,
// then a summary line; the exit code is the number of failures (0 = all passed).
#include <cstdio>

namespace portable_check {
inline int& Failures() { static int n = 0; return n; }
inline int& Count() { static int n = 0; return n; }
inline bool& Quiet() { static bool q = false; return q; }   // only failures are printed
// stdout flushed after every check line (a test with threads or ExitProcess, so a line is out before a crash);
// off unless a test sets it: static const bool kFlush = (portable_check::FlushEach() = true);
inline bool& FlushEach() { static bool f = false; return f; }
}

inline bool Check(bool ok, const char* what) {
    ++portable_check::Count();
    if (!ok) ++portable_check::Failures();
    if (!ok || !portable_check::Quiet()) std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (portable_check::FlushEach()) std::fflush(stdout);
    return ok;
}

inline int Summary(const char* name) {
    int f = portable_check::Failures();
    std::printf("%s: %s (%d checks, %d failure%s)\n", name, f ? "FAILED" : "ALL PASSED", portable_check::Count(), f, f == 1 ? "" : "s");
    return f;
}
