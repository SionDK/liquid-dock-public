// The installed elevated helper's window service, live (tests\run_admin_live.bat), seen from outside as
// any program of the user sees it - what the in-process tests (admin_helper_test, admin_service_edge_test)
// cannot show, because there both sides run at the same level in one process:
//   - the helper in %ProgramFiles%\LiquidDock has the window service (and how old it is next to the copy
//     the dock ships: the dock replaces only a helper older than the service, admin_protocol.h)
//   - it answers on this session's pipe and is the trusted one
//   - it refuses a program that is not Liquid Dock (this test)
//   - nobody takes its pipe's name next to it (a second instance)
//   - a low-integrity program can neither open the pipe to send nor open it at all (the second checked
//     unless the installed helper's file holds only the older pipe label: a helper is kept while it has
//     the window service, so one from before the label said so may still be installed)
//   - it runs above this program's level: nothing can be written into it from here (whether it can be ended is
//     printed: Windows lets the user's programs end it)
//   - its sensor memory can be read, not written, from here
//   - how long one request takes (the dock asks on its UI thread, right after a click), also while every
//     processor is busy (about 1 s of full load from this test's own threads)
// It sends only Hello (no window is touched); each request is a line "refused a request from ..." in the
// helper's %ProgramFiles%\LiquidDock\sensors.log. Needs the helper running: the dock with the option
// 설정 -> 시스템 -> '관리자 권한 앱 창 제어' on (or the CPU temperature from the helper).
//   cl /std:c++20 /EHsc /utf-8 tests\admin_live_pipe_test.cpp /link user32.lib advapi32.lib shell32.lib ole32.lib version.lib
#include "../src/admin_client.h"
#include "portable_check.h"
#include <sddl.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace adminhelper;

namespace {

std::wstring SelfPath() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, ARRAYSIZE(p));
    return std::wstring(p, n);
}

std::wstring InstalledHelper() {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p))) { s = p; CoTaskMemFree(p); }
    return s + L"\\LiquidDock\\LiquidDockSensors.exe";
}

// The running installed helper's process id (0: none).
DWORD HelperPid(const std::wstring& installed) {
    DWORD found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"LiquidDockSensors.exe") && SameNoCase(ImageOfProcess(pe.th32ProcessID), installed)) found = pe.th32ProcessID;
    CloseHandle(snap);
    return found;
}

// Whether a helper file holds the window service's pipe label that keeps programs below medium integrity
// from opening the pipe at all (admin_service.h: no write up and no read up), as the compiler keeps the
// text (UTF-16, one piece): 1 it does, 0 it does not (a helper from before that label: no write up only),
// -1 the file could not be read. The version cannot tell: a helper is built from whatever source its dock was.
int HasNoReadUpLabel(const std::wstring& file) {
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size = {};
    std::vector<char> bytes;
    DWORD got = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64ll << 20)) {
        bytes.resize((size_t)size.QuadPart);
        if (!ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr)) got = 0;
    }
    CloseHandle(f);
    if (!got || got != bytes.size()) return -1;
    static const wchar_t label[] = L"S:(ML;;NWNR;;;ME)";
    const char* l = (const char*)label;
    return std::search(bytes.begin(), bytes.end(), l, l + sizeof(label) - sizeof(wchar_t)) != bytes.end() ? 1 : 0;
}

// This test again, at low integrity: its exit code is the Windows error of opening the pipe (0: opened).
DWORD RunLow(const std::wstring& args) {
    DWORD code = ~0u;
    HANDLE own = nullptr, low = nullptr;
    PSID sid = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &own) &&
        DuplicateTokenEx(own, 0, nullptr, SecurityImpersonation, TokenPrimary, &low) && ConvertStringSidToSidW(L"S-1-16-4096", &sid)) {
        TOKEN_MANDATORY_LABEL label = {};
        label.Label.Attributes = SE_GROUP_INTEGRITY;
        label.Label.Sid = sid;
        std::wstring cmd = L"\"" + SelfPath() + L"\" " + args;
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (SetTokenInformation(low, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid)) &&
            CreateProcessAsUserW(low, nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            if (WaitForSingleObject(pi.hProcess, 8000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
            else TerminateProcess(pi.hProcess, ~0u);   // this test's own child
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    if (sid) LocalFree(sid);
    if (low) CloseHandle(low);
    if (own) CloseHandle(own);
    return code;
}

}   // namespace

int wmain(int argc, wchar_t** argv) {
    // the low-integrity copy: --open <pipe> read|write
    if (argc >= 4 && !wcscmp(argv[1], L"--open")) {
        DWORD access = !wcscmp(argv[3], L"write") ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
        HANDLE h = CreateFileW(argv[2], access, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return (int)GetLastError();
        CloseHandle(h);
        return 0;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const std::wstring pipe = PipeName(session), installed = InstalledHelper();
    std::printf("pipe %ls, installed helper %ls, this test at level 0x%lx\n", pipe.c_str(), installed.c_str(), OwnIntegrity());

    const DWORD pid = HelperPid(installed);
    Check(GetFileAttributesW(installed.c_str()) != INVALID_FILE_ATTRIBUTES, "the helper is installed in %ProgramFiles%\\LiquidDock");
    // the installed helper against the one the installed dock ships (%LOCALAPPDATA%\Programs\LiquidDock)
    auto versionOf = [](const std::wstring& file) -> uint64_t {
        DWORD dummy = 0, size = GetFileVersionInfoSizeW(file.c_str(), &dummy);
        if (!size) return 0;
        std::vector<BYTE> buf(size);
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (!GetFileVersionInfoW(file.c_str(), 0, size, buf.data()) || !VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || len < sizeof(*fi)) return 0;
        return PackVersion(fi->dwFileVersionMS, fi->dwFileVersionLS);
    };
    PWSTR local = nullptr;
    std::wstring shipped;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProgramFiles, 0, nullptr, &local))) { shipped = local; CoTaskMemFree(local); }
    shipped += L"\\LiquidDock\\LiquidDockSensors.exe";
    const uint64_t vi = versionOf(installed), vs = versionOf(shipped);
    std::printf("  (installed helper 0.%llu.%llu.%llu, the dock's own copy 0.%llu.%llu.%llu)\n", (vi >> 32) & 0xFFFF, (vi >> 16) & 0xFFFF, vi & 0xFFFF,
                (vs >> 32) & 0xFFFF, (vs >> 16) & 0xFFFF, vs & 0xFFFF);
    // an older one than the dock's copy is kept while it has the window service (an update of the dock alone
    // asks for no UAC prompt): what changed in the helper since then reaches it only with a new install
    Check(HelperCurrent(vi), "the installed helper has the window service (not older than mod.10)");
    if (vi && vs && vi < vs) std::printf("  (the installed helper is older than the dock's copy: kept, as it has the window service)\n");
    Check(pid != 0 && PipeExists(pipe), "it runs and its pipe is there (the dock with the admin-window option on)");
    if (!pid || !PipeExists(pipe)) return Summary("admin_live_pipe_test");

    // who answers, and whom it serves
    Result r = Call(pipe, Command::Hello, 0, installed);
    std::printf("  (Hello: %ls)\n", ResultName(r));
    Check(r == Result::WrongClient, "the installed helper answers (trusted) and refuses this test: not Liquid Dock");

    // the pipe's name
    HANDLE second = CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 256, 256, 0, nullptr);
    DWORD secondError = second == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
    std::printf("  (a second pipe of that name: error %lu)\n", secondError);
    Check(second == INVALID_HANDLE_VALUE, "no program of the user can put a second pipe of that name next to it");

    // low integrity
    DWORD write = RunLow(L"--open " + pipe + L" write"), read = RunLow(L"--open " + pipe + L" read");
    // whether the installed helper is meant to refuse the second: its file, not its version (see HasNoReadUpLabel)
    const int label = HasNoReadUpLabel(installed), shippedLabel = HasNoReadUpLabel(shipped);
    auto labelName = [](int l) { return l > 0 ? "no write up, no read up" : l == 0 ? "no write up only (older)" : "file not read"; };
    std::printf("  (a low-integrity program opening the pipe: to send %lu, to read only %lu; pipe label in the installed helper:\n"
                "   %s, in the dock's copy: %s)\n", write, read, labelName(label), labelName(shippedLabel));
    Check(write == ERROR_ACCESS_DENIED, "a low-integrity program cannot open it to send requests");
    if (label != 0)
        Check(read == ERROR_ACCESS_DENIED, "nor open it at all (to read only: it would hold the helper's one connection)");
    else
        std::printf("  (to read only: not checked - the installed helper is from before its pipe kept low-integrity programs from\n"
                    "   opening it at all; %s)\n",
                    shippedLabel > 0 ? "the dock's copy has that: a new install of the helper brings it (LiquidDockSensors.exe --install)"
                    : shippedLabel == 0 ? "so is the dock's copy: a dock with that label comes first, then a new install of the helper"
                                        : "the dock's copy was not found");

    // elevated
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    HANDLE t = nullptr;
    DWORD level = p && OpenProcessToken(p, TOKEN_QUERY, &t) ? IntegrityOf(t) : 0;
    if (t) CloseHandle(t);
    if (p) CloseHandle(p);
    std::printf("  (the helper's level: 0x%lx)\n", level);
    Check(level == 0 || level > OwnIntegrity(), "the helper runs above this program's level (or its level cannot be read from here)");
    // Windows lets a program of the same user end an elevated process (a 25H2 PC, 10-28: PROCESS_TERMINATE
    // opened, the memory rights did not): that only stops the helper, which the dock starts again; what matters is
    // that nothing can be written into it
    HANDLE end = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    HANDLE poke = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    std::printf("  (ending it from here: %s)\n", end ? "allowed, as for any elevated program of this user" : "refused");
    Check(!poke, "nothing can be written into it from here");
    if (end) CloseHandle(end);   // only asked whether it could be: never used
    if (poke) CloseHandle(poke);

    // the sensor memory
    HANDLE w = OpenFileMappingW(FILE_MAP_WRITE, FALSE, L"Local\\LiquidDockSensors");
    DWORD wError = w ? 0 : GetLastError();
    if (w) CloseHandle(w);
    HANDLE rd = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\LiquidDockSensors");
    Check(!w && wError == ERROR_ACCESS_DENIED, "its sensor memory cannot be written from here");
    Check(rd != nullptr, "and can be read (the dock's CPU temperature)");
    if (rd) CloseHandle(rd);

    // one request's time (on the dock's UI thread for every click on an administrator's window)
    ULONGLONG longest = 0, total = 0;
    int answered = 0;
    for (int i = 0; i < 10; ++i) {
        ULONGLONG s = GetTickCount64();
        if (Call(pipe, Command::Hello, 0, installed) == Result::WrongClient) ++answered;
        ULONGLONG d = GetTickCount64() - s;
        total += d;
        if (d > longest) longest = d;
    }
    std::printf("  (10 requests: %d answered, %.1f ms on average, longest %llu ms)\n", answered, total / 10.0, (unsigned long long)longest);
    Check(answered == 10 && longest < 500, "ten requests in a row: each answered in well under the dock's bound");

    // the same with every processor busy at normal priority (a game, an encoder, a build): the helper's
    // scheduled task starts it below normal (Priority 7), the dock waits 1.5 s for an answer
    p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    DWORD cls = p ? GetPriorityClass(p) : 0;
    if (p) CloseHandle(p);
    std::printf("  (the helper's priority class: 0x%lx%s)\n", cls, cls == BELOW_NORMAL_PRIORITY_CLASS ? ", below normal" : "");
    std::atomic<bool> busy{ true };
    std::vector<std::thread> load;
    DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    for (DWORD i = 0; i < cpus; ++i)
        load.emplace_back([&] { volatile unsigned long long x = 0; while (busy) x = x + 1; });
    Sleep(300);
    longest = 0;
    answered = 0;
    for (int i = 0; i < 8; ++i) {
        ULONGLONG s = GetTickCount64();
        if (Call(pipe, Command::Hello, 0, installed) == Result::WrongClient) ++answered;
        ULONGLONG d = GetTickCount64() - s;
        if (d > longest) longest = d;
        Sleep(100);
    }
    busy = false;
    for (auto& th : load) th.join();
    std::printf("  (with %lu processors busy: %d of 8 answered, longest %llu ms)\n", cpus, answered, (unsigned long long)longest);
    // 10-28, on a test PC: 1 of 8 lost (1515 ms) in 4 runs of 5 until the helper's thread ran at the dock's level
    Check(answered == 8 && longest < 1500, "with every processor busy: each request still answered within the dock's 1.5 s");
    return Summary("admin_live_pipe_test");
}
