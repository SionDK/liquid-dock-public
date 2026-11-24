#pragma once
// What the Dock's parts must never do in a test (tests\parts_dock.inc includes this before them and names these
// fakes in its #define list): log off, restart or shut down Windows (ExitWindowsEx), lock it (LockWorkStation), put
// it to sleep (LoadLibraryExW of powrprof.dll for SetSuspendState). The parts reach them only from a menu item
// that a test's TrackPopupMenu fake picks; none does now. A call prints a FAIL line and ends the test at once with
// exit code 3 (nothing else runs, so no check after it can pass over it).
// LoadLibraryExW of any other DLL goes on to the real ::LoadLibraryExW (as without the fake): only powrprof.dll, by its
// name or a path that ends in it (any case), is refused, and the FAIL line names the DLL it was given.
// Included under the test's fakes: it uses nothing a test renames, and no header of its own (<cstdio> and
// <windows.h> come before it in every dock test). It comes before parts_dock.inc's '#define LoadLibraryExW', so
// ::LoadLibraryExW here is the real one.
[[noreturn]] static void DockFixtureForbidden(const char* call, const char* what, const wchar_t* arg = nullptr) {
    if (arg)
        printf("FAIL: dock fixture: the Dock called %s(\"%ls\") - %s; a test must never do that\n", call, arg, what);
    else
        printf("FAIL: dock fixture: the Dock called %s - %s; a test must never do that\n", call, what);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}
static BOOL WINAPI DockFixtureExitWindowsEx(UINT, DWORD) { DockFixtureForbidden("ExitWindowsEx", "logging off, restarting or shutting down Windows"); }
static BOOL WINAPI DockFixtureLockWorkStation() { DockFixtureForbidden("LockWorkStation", "locking Windows"); }
// Whether name is powrprof.dll or a path ending in \powrprof.dll or /powrprof.dll (ASCII case folded by hand: no
// CRT or Windows call a test might rename).
static bool DockFixtureIsPowrProf(const wchar_t* name) {
    if (!name) return false;
    static const wchar_t want[] = L"powrprof.dll";
    const size_t wantLen = sizeof(want) / sizeof(want[0]) - 1;
    size_t len = 0;
    while (name[len]) ++len;
    if (len < wantLen) return false;
    const wchar_t* tail = name + (len - wantLen);
    if (tail != name && tail[-1] != L'\\' && tail[-1] != L'/') return false;
    for (size_t i = 0; i < wantLen; ++i) {
        wchar_t c = tail[i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        if (c != want[i]) return false;
    }
    return true;
}
static HMODULE WINAPI DockFixtureLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    if (DockFixtureIsPowrProf(name))
        DockFixtureForbidden("LoadLibraryExW", "loading powrprof.dll to put the PC to sleep", name);
    return ::LoadLibraryExW(name, file, flags);
}
