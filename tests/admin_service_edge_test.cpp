// The admin window service's edges (src/admin_service.h, the elevated helper's side, and src/admin_client.h,
// the dock's side) over named pipes of its own: what the dock does when the program on the pipe never
// answers, answers too much or answers with something else, how long it waits for a pipe that stays busy,
// and what the helper does with messages that are too long or cut short, with many clients at once, with a
// restart, and with a client that came and went before the helper listened. Every check uses its own pipe
// (names with "hunt-admin" and this process id) and no window at all: no other program, no foreground.
//   cl /std:c++20 /EHsc /utf-8 tests\admin_service_edge_test.cpp /link user32.lib advapi32.lib
#define LD_ADMIN_CLIENT_EXE L"admin_service_edge_test.exe"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <string>

// ConnectNamedPipe as the helper calls it, with (when a check asks) a client that opens the pipe and closes
// it again just before - the moment between the helper creating its pipe and listening on it - or a failure.
namespace hook {
int comeAndGo = 0;
std::wstring pipe;
std::atomic<int> fail{ 0 };   // the next calls that fail (set while the service runs)
}
static BOOL WINAPI HookedConnectNamedPipe(HANDLE p, LPOVERLAPPED ov) {
    if (hook::fail > 0) {
        --hook::fail;
        SetLastError(ERROR_BAD_PIPE);
        return FALSE;
    }
    if (hook::comeAndGo > 0) {
        --hook::comeAndGo;
        HANDLE c = CreateFileW(hook::pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (c != INVALID_HANDLE_VALUE) CloseHandle(c);
    }
    return ConnectNamedPipe(p, ov);
}
#define ConnectNamedPipe HookedConnectNamedPipe
#include "../src/admin_service.h"
#undef ConnectNamedPipe
#include "../src/admin_client.h"
#include "portable_check.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace adminhelper;

namespace {

std::wstring SelfPath() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, ARRAYSIZE(p));
    return std::wstring(p, n);
}

std::wstring PipeOf(const wchar_t* what) {
    return L"\\\\.\\pipe\\LiquidDockTest.hunt-admin." + std::to_wstring(GetCurrentProcessId()) + L"." + what;
}

template <class F> bool WaitFor(F&& ok, DWORD ms = 3000) {
    ULONGLONG until = GetTickCount64() + ms;
    while (!ok()) {
        if (GetTickCount64() > until) return false;
        Sleep(20);
    }
    return true;
}

// A program on the helper's pipe that is not the helper's window service: it takes one client, reads its
// request and answers as told (or not at all), then waits until the client is gone.
struct RawServer {
    enum Mode { Silent, TooLong, DockSide, OtherMagic };
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::thread t;
    void Start(const std::wstring& name, Mode mode) {
        pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                1, 256, 256, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        t = std::thread([this, mode] {
            if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
            unsigned char buf[256];
            DWORD got = 0, sent = 0;
            if (!ReadFile(pipe, buf, sizeof(buf), &got, nullptr)) return;
            if (mode == TooLong) {
                unsigned char big[32] = {};
                Reply r = MakeReply(Result::Ok);
                std::memcpy(big, &r, sizeof(r));
                WriteFile(pipe, big, sizeof(big), &sent, nullptr);
            } else if (mode == DockSide) {
                Reply r = MakeReply(Result::NotRunning);
                WriteFile(pipe, &r, sizeof(r), &sent, nullptr);
            } else if (mode == OtherMagic) {
                Reply r = MakeReply(Result::Ok);
                r.magic = 0x12345678u;
                WriteFile(pipe, &r, sizeof(r), &sent, nullptr);
            }
            ReadFile(pipe, buf, sizeof(buf), &got, nullptr);   // until the client has closed
        });
    }
    void Stop() {
        if (pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe, nullptr);
            DisconnectNamedPipe(pipe);
        }
        if (t.joinable()) t.join();
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }
};

// One message from a client that is not the dock's Call: written, then the answer read, each at most `ms`.
// Returns the bytes of the answer (0: none came).
DWORD RawExchange(const std::wstring& pipe, const void* msg, DWORD size, Reply& out, DWORD ms = 2000) {
    HANDLE h = INVALID_HANDLE_VALUE;
    ULONGLONG until = GetTickCount64() + ms;
    while ((h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)) == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_PIPE_BUSY || GetTickCount64() >= until) return 0;
        WaitNamedPipeW(pipe.c_str(), 200);
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // one overlapped read or write, at most ms long (false: failed, or cancelled after ms)
    auto io = [&](bool write, void* buf, DWORD n, DWORD& done) {
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);
        done = 0;
        BOOL ok = write ? WriteFile(h, buf, n, nullptr, &ov) : ReadFile(h, buf, n, nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(ev, ms) != WAIT_OBJECT_0) {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, &done, TRUE);
            done = 0;
            return false;
        }
        return GetOverlappedResult(h, &ov, &done, FALSE) != FALSE;
    };
    DWORD sent = 0, got = 0;
    if (!io(true, const_cast<void*>(msg), size, sent) || !io(false, &out, sizeof(out), got)) got = 0;
    CloseHandle(ev);
    CloseHandle(h);
    return got;
}

// The service on its own thread, as the helper's loop runs it between its ticks.
struct Running {
    Service service;
    std::atomic<bool> stop{ false };
    std::thread t;
    DWORD pollMs = 50;   // how long the loop lets it wait (5 s between the passes of a helper serving windows only)
    bool Start(const std::wstring& name, Service::LogFn log = nullptr) {
        bool ok = service.Start(name, log);
        t = std::thread([this] { while (!stop) service.Poll(pollMs); });
        return ok;
    }
    void Stop() {
        stop = true;
        if (t.joinable()) t.join();
        service.Stop();
    }
};

std::mutex logLock;
std::vector<std::wstring> logs;
void Collect(const wchar_t* line) {
    std::lock_guard<std::mutex> g(logLock);
    logs.emplace_back(line);
    std::printf("  log: %ls\n", line);
}
bool Said(const wchar_t* a) {
    std::lock_guard<std::mutex> g(logLock);
    for (const auto& l : logs)
        if (l.find(a) != std::wstring::npos) return true;
    return false;
}

// This test again, at low integrity (as a sandboxed program runs): its exit code is the Windows error of
// what it was asked to do (0: done), ~0u when it could not be started.
DWORD RunLow(const std::wstring& args, DWORD waitMs = 8000) {
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
            if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
            else TerminateProcess(pi.hProcess, ~0u);   // this test's own child
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            std::printf("  (low-integrity copy not started: error %lu)\n", GetLastError());
        }
    }
    if (sid) LocalFree(sid);
    if (low) CloseHandle(low);
    if (own) CloseHandle(own);
    return code;
}

// The low-integrity copy's part: --open <pipe> read|write [hold ms], --create <pipe>, --level.
int Child(int argc, wchar_t** argv) {
    if (!wcscmp(argv[1], L"--level")) {
        HANDLE t = nullptr;
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t);
        DWORD level = t ? IntegrityOf(t) : 0;
        if (t) CloseHandle(t);
        return (int)level;
    }
    if (!wcscmp(argv[1], L"--open") && argc >= 4) {
        DWORD access = !wcscmp(argv[3], L"write") ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
        HANDLE h = CreateFileW(argv[2], access, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return (int)GetLastError();
        if (argc >= 5) Sleep((DWORD)_wtoi(argv[4]));
        CloseHandle(h);
        return 0;
    }
    if (!wcscmp(argv[1], L"--create") && argc >= 3) {
        HANDLE h = CreateNamedPipeW(argv[2], PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 256, 256, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return (int)GetLastError();
        CloseHandle(h);
        return 0;
    }
    return 1;
}

}   // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && argv[1][0] == L'-') return Child(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // every line at once (a check that hangs shows where)
    const std::wstring self = SelfPath();

    // ---- the dock's side against a program that is not the window service (it passes the trust check:
    //      the test is that program) - every answer is "no answer", in bounded time
    struct { RawServer::Mode mode; const char* what; } wrong[] = {
        { RawServer::Silent, "a program that reads the request and never answers: no answer, within the dock's bound" },
        { RawServer::TooLong, "an answer longer than a reply: no answer (nothing read past the reply)" },
        { RawServer::DockSide, "an answer the helper never gives (a dock-side result): no answer" },
        { RawServer::OtherMagic, "an answer of another protocol: no answer" },
    };
    for (auto& w : wrong) {
        const std::wstring pipe = PipeOf(L"raw");
        RawServer s;
        s.Start(pipe, w.mode);
        ULONGLONG t0 = GetTickCount64();
        Result r = Call(pipe, Command::Hello, 0, self);
        ULONGLONG took = GetTickCount64() - t0;
        Check(s.pipe != INVALID_HANDLE_VALUE && r == Result::NoAnswer && took < 2500, w.what);
        if (r != Result::NoAnswer || took >= 2500) std::printf("  (answer: %ls after %llu ms)\n", ResultName(r), (unsigned long long)took);
        s.Stop();
    }

    // ---- a pipe whose only instance stays busy with another client: the dock waits waitMs, not more
    {
        const std::wstring pipe = PipeOf(L"busy");
        HANDLE server = CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 256, 256, 0, nullptr);
        HANDLE other = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(server != INVALID_HANDLE_VALUE && other != INVALID_HANDLE_VALUE, "(a pipe held by another client)");
        Check(PipeExists(pipe), "a busy pipe is there for the dock (it does not start a second helper)");
        ULONGLONG t0 = GetTickCount64();
        Result r = Call(pipe, Command::Hello, 0, self, 300);
        ULONGLONG took = GetTickCount64() - t0;
        Check(r == Result::NoAnswer && took >= 250 && took < 1000, "a busy helper: no answer after the dock's wait (300 ms here), not later");
        std::printf("  (waited %llu ms)\n", (unsigned long long)took);
        if (other != INVALID_HANDLE_VALUE) CloseHandle(other);
        if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
        Check(Call(pipe, Command::Hello, 0, self) == Result::NotRunning, "no pipe any more: not running, at once");
    }

    // ---- the window service against clients that are not the dock
    {
        const std::wstring pipe = PipeOf(L"service");
        Running run;
        Check(run.Start(pipe, Collect), "the window service is up");

        // longer than any request (and than the pipe's buffer): dropped without an answer, the next one served
        unsigned char big[300] = {};
        Request req = MakeRequest(Command::Hello, 0);
        std::memcpy(big, &req, sizeof(req));
        Reply rep{}, parsed{};
        DWORD got = RawExchange(pipe, big, sizeof(big), rep);
        Check(got == 0 || (ParseReply(&rep, got, parsed) && parsed.result == (uint32_t)Result::BadRequest), "a message longer than any request is not served");
        ULONGLONG t0 = GetTickCount64();
        Check(Call(pipe, Command::Hello, 0, self) == Result::Ok && GetTickCount64() - t0 < 2000, "and the next client is served at once");

        // cut short: a bad request, said in the log, the next one served
        got = RawExchange(pipe, &req, 10, rep);
        Check(got == sizeof(Reply) && ParseReply(&rep, got, parsed) && parsed.result == (uint32_t)Result::BadRequest, "a request cut short: a bad request");
        Check(WaitFor([] { return Said(L"(10 bytes)"); }), "and the log says how long it was");
        // a request and gone before the answer: the helper does not wait for it
        {
            HANDLE h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE && WaitNamedPipeW(pipe.c_str(), 2000))
                h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            DWORD sent = 0;
            Request hello = MakeRequest(Command::Hello, 0);
            if (h != INVALID_HANDLE_VALUE) { WriteFile(h, &hello, sizeof(hello), &sent, nullptr); CloseHandle(h); }
            t0 = GetTickCount64();
            Check(h != INVALID_HANDLE_VALUE && Call(pipe, Command::Hello, 0, self) == Result::Ok && GetTickCount64() - t0 < 1500,
                  "a client gone before its answer does not hold the helper up");
        }

        // many docks' worth of clients at once: every one answered, none waiting longer than the dock's bound
        std::atomic<int> ok{ 0 }, bad{ 0 };
        std::atomic<unsigned long long> longest{ 0 };
        std::vector<std::thread> clients;
        for (int i = 0; i < 6; ++i)
            clients.emplace_back([&] {
                for (int k = 0; k < 4; ++k) {
                    ULONGLONG s = GetTickCount64();
                    Result r = Call(pipe, Command::Hello, 0, self);
                    unsigned long long d = GetTickCount64() - s;
                    if (d > longest) longest = d;
                    (r == Result::Ok ? ok : bad)++;
                }
            });
        for (auto& c : clients) c.join();
        Check(ok == 24 && bad == 0, "24 requests from 6 clients at once: all answered");
        Check(longest < 3000, "none waited longer than the dock's bound");
        std::printf("  (%d answered, %d not; longest %llu ms)\n", ok.load(), bad.load(), longest.load());
        run.Stop();
        Check(!PipeExists(pipe), "the pipe goes with the service");

        // started again (the helper's Start after a Stop): the same name, served again
        Running again;
        Check(again.Start(pipe, Collect) && Call(pipe, Command::Hello, 0, self) == Result::Ok, "a service started again on the same name is served");
        again.Stop();
    }

    // ---- a client that opened the pipe and closed it again before the helper listened on it (the helper
    //      had just created the pipe): the helper still serves the next one
    {
        const std::wstring pipe = PipeOf(L"cameandwent");
        hook::pipe = pipe;
        hook::comeAndGo = 1;
        Running run;
        bool started = run.Start(pipe, Collect);
        Check(hook::comeAndGo == 0, "(a client came and went before the helper listened)");
        Check(started, "the window service still starts");
        ULONGLONG t0 = GetTickCount64();
        Result r = Call(pipe, Command::Hello, 0, self);
        ULONGLONG took = GetTickCount64() - t0;
        Check(r == Result::Ok, "and the dock's next request is answered (not 'no answer' for as long as the helper runs)");
        std::printf("  (answer: %ls after %llu ms)\n", ResultName(r), (unsigned long long)took);
        Check(Call(pipe, Command::Hello, 0, self) == Result::Ok, "and the one after");
        Check(Said(L"came and went before it listened"), "and the helper's log says what happened");
        run.Stop();
    }

    // ---- the pipe did not listen again after a request (ConnectNamedPipe failed, then once more): listened
    //      on again within a second, though the helper's loop lets it wait 5 s - the dock waits 1.5 s for it
    {
        const std::wstring pipe = PipeOf(L"nolisten");
        Running run;
        run.pollMs = 5000;
        Check(run.Start(pipe, Collect), "the window service is up");
        hook::fail = 2;   // after the next request, and on the next try
        Check(Call(pipe, Command::Hello, 0, self) == Result::Ok, "a request is answered");
        ULONGLONG t0 = GetTickCount64();
        Result r = Call(pipe, Command::Hello, 0, self);
        ULONGLONG took = GetTickCount64() - t0;
        std::printf("  (after two failed listens: %ls after %llu ms; was: no answer after 1,500 ms, listened again after 5 s)\n",
                    ResultName(r), (unsigned long long)took);
        Check(hook::fail == 0, "(the pipe did not listen, twice)");
        Check(r == Result::Ok && took < 1500, "the pipe did not listen: the dock's next request is still answered (listened again within 1 s)");
        run.stop = true;
        Call(pipe, Command::Hello, 0, self, 300);   // wakes its wait
        run.Stop();
    }

    // ---- low-integrity programs (sandboxed browsers and the like): the pipe is the same user's, but its
    //      label keeps them out - not only from sending, also from taking the helper's one connection
    {
        const std::wstring pipe = PipeOf(L"lowil");
        Running run;
        Check(run.Start(pipe, Collect), "the window service is up");
        DWORD level = RunLow(L"--level");
        Check(level == 0x1000, "(a low-integrity copy of the test runs)");
        if (level == 0x1000) {
            Check(RunLow(L"--open " + pipe + L" write") == ERROR_ACCESS_DENIED, "a low-integrity program cannot open the pipe to send requests");
            Check(RunLow(L"--create " + pipe) != 0, "nor put a second pipe of the same name next to it");
            DWORD readOnly = RunLow(L"--open " + pipe + L" read");
            std::printf("  (opening it to read only: %s, error %lu)\n", readOnly == 0 ? "opened" : "refused", readOnly);
            Check(readOnly == ERROR_ACCESS_DENIED, "nor open it at all (to read only) - a client the helper would wait for");
            // while a low-integrity program holds the one connection, the dock's request waits for it
            std::thread holder([&] { RunLow(L"--open " + pipe + L" read 900"); });
            Sleep(300);
            ULONGLONG t0 = GetTickCount64();
            Result r = Call(pipe, Command::Hello, 0, self, 300);
            ULONGLONG took = GetTickCount64() - t0;
            holder.join();
            std::printf("  (the dock's request while it held the pipe: %ls after %llu ms)\n", ResultName(r), (unsigned long long)took);
            Check(r == Result::Ok, "a low-integrity program cannot keep the dock's request waiting");
        }
        run.Stop();
    }

    return Summary("admin_service_edge_test");
}
