// The admin window service end to end (src/admin_service.h, the elevated helper's side, and
// src/admin_client.h, the dock's side) over a real named pipe of its own, on windows of its own:
// restore and bring up, minimize, close; windows without a taskbar button refused; a program other
// than the expected one refused on both sides; a client that connects and stays silent does not hold
// the helper up. Touches no other program's window. (Windows keeps a non-elevated program from
// minimizing an elevated one's window - UIPI - which only real Windows shows; here both sides run
// in one process.)
//   cl /std:c++20 /EHsc /utf-8 tests\admin_helper_test.cpp user32.lib advapi32.lib
#define LD_ADMIN_CLIENT_EXE L"admin_helper_test.exe"
#include "../src/admin_service.h"
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

template <class F> bool WaitFor(F&& ok, DWORD ms = 3000) {
    ULONGLONG until = GetTickCount64() + ms;
    while (!ok()) {
        if (GetTickCount64() > until) return false;
        Sleep(20);
    }
    return true;
}

// An "app": its windows on their own thread with a message loop, as a real program has.
struct App {
    HWND main = nullptr, tool = nullptr, owned = nullptr, ownedApp = nullptr, child = nullptr, hidden = nullptr;
    DWORD thread = 0;
    std::thread t;
    void Start() {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        t = std::thread([this, ready] {
            thread = GetCurrentThreadId();
            WNDCLASSW wc = {};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"LiquidDockAdminTestApp";
            RegisterClassW(&wc);
            auto make = [&](DWORD ex, DWORD style, HWND parent, const wchar_t* title) {
                return CreateWindowExW(ex, wc.lpszClassName, title, style, 100, 100, 320, 200, parent, nullptr, wc.hInstance, nullptr);
            };
            main = make(0, WS_OVERLAPPEDWINDOW | WS_VISIBLE, nullptr, L"admin test app");
            tool = make(WS_EX_TOOLWINDOW, WS_OVERLAPPEDWINDOW | WS_VISIBLE, nullptr, L"tool window");
            owned = make(0, WS_OVERLAPPEDWINDOW | WS_VISIBLE, main, L"owned dialog");
            ownedApp = make(WS_EX_APPWINDOW, WS_OVERLAPPEDWINDOW | WS_VISIBLE, main, L"owned app window");
            child = make(0, WS_CHILD | WS_VISIBLE, main, L"child");
            hidden = make(0, WS_OVERLAPPEDWINDOW, nullptr, L"hidden window");
            SetEvent(ready);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
            for (HWND h : { child, ownedApp, owned, tool, hidden, main }) if (IsWindow(h)) DestroyWindow(h);
        });
        WaitForSingleObject(ready, 5000);
        CloseHandle(ready);
    }
    void Stop() {
        PostThreadMessageW(thread, WM_QUIT, 0, 0);
        if (t.joinable()) t.join();
    }
};

uint64_t W(HWND h) { return (uint64_t)(uintptr_t)h; }

}   // namespace

int main() {
    const std::wstring pipe = PipeName(GetCurrentProcessId(), L"\\\\.\\pipe\\LiquidDockTest.AdminHelper.");
    const std::wstring self = SelfPath();

    Check(!PipeExists(pipe), "no helper yet: no pipe");
    ULONGLONG t0 = GetTickCount64();
    Check(Call(pipe, Command::Hello, 0, self) == Result::NotRunning && GetTickCount64() - t0 < 500, "no helper: the dock knows at once");

    Service service;
    // the service's lines, written on its thread: it answers first and says why after (admin_service.h), so a
    // line is waited for, not read the moment the answer is in
    static std::mutex logLock;
    static std::vector<std::wstring> logs;
    Check(service.Start(pipe, [](const wchar_t* line) {
              { std::lock_guard<std::mutex> g(logLock); logs.emplace_back(line); }
              std::printf("  log: %ls\n", line);
          }), "the helper's pipe is up");
    auto said = [](const wchar_t* a, const wchar_t* b) {
        std::lock_guard<std::mutex> g(logLock);
        for (const auto& l : logs)
            if (l.find(a) != std::wstring::npos && l.find(b) != std::wstring::npos) return true;
        return false;
    };
    Service squatter;
    Check(!squatter.Start(pipe), "a second program cannot take the same pipe name (first instance only)");
    std::atomic<bool> stop{ false };
    std::thread server([&] { while (!stop) service.Poll(50); });
    Check(PipeExists(pipe), "the dock sees the pipe without connecting");

    App app;
    app.Start();
    Check(IsWindow(app.main) && IsWindow(app.tool) && IsWindow(app.child), "the test app's windows");

    Check(Call(pipe, Command::Hello, 0, self) == Result::Ok, "Hello");

    // windows without a taskbar button are left alone
    Check(Call(pipe, Command::Minimize, W(app.tool), self) == Result::NotAllowed, "a tool window is refused");
    Check(Call(pipe, Command::Close, W(app.owned), self) == Result::NotAllowed, "an owned dialog is refused");
    Check(Call(pipe, Command::Minimize, W(app.child), self) == Result::NotAllowed, "a child window is refused");
    Check(Call(pipe, Command::Activate, W(app.hidden), self) == Result::NotAllowed, "a hidden window is refused");
    Check(IsWindow(app.owned) && !IsIconic(app.tool), "(and stay as they were)");
    Result ownedApp = Call(pipe, Command::Minimize, W(app.ownedApp), self);
    Check(ownedApp == Result::Ok && WaitFor([&] { return IsIconic(app.ownedApp) != FALSE; }),
          "an owned window marked as an app window has a button: served");
    if (ownedApp != Result::Ok) std::printf("  (answer: %ls)\n", ResultName(ownedApp));
    Check(Call(pipe, Command::Activate, 0x7FFF0001ull, self) == Result::NoWindow, "not a window");
    Check(Call(pipe, Command::Activate, 0, self) == Result::BadRequest, "no window at all: a bad request");

    // restore and bring up, minimize
    ShowWindow(app.main, SW_MINIMIZE);
    Check(WaitFor([&] { return IsIconic(app.main) != FALSE; }), "(the app window is minimized)");
    Check(Call(pipe, Command::Activate, W(app.main), self) == Result::Ok, "Activate: done");
    Check(WaitFor([&] { return !IsIconic(app.main); }), "Activate restores a minimized window");
    Check(Call(pipe, Command::Minimize, W(app.main), self) == Result::Ok, "Minimize: done");
    Check(WaitFor([&] { return IsIconic(app.main) != FALSE; }), "Minimize minimizes it (as its own minimize button)");
    Check(Call(pipe, Command::Activate, W(app.main), self) == Result::Ok && WaitFor([&] { return !IsIconic(app.main); }), "and back up again");

    // Activate takes the front itself (mod.24 BringToFront: its own key for the right, then attaching to the
    // thread in front when Windows still says no) - none of it waits on a program, so a minimized window of a
    // program that does not answer its messages is answered at once too, and comes up once that program does
    ShowWindow(app.main, SW_MINIMIZE);
    Check(WaitFor([&] { return IsIconic(app.main) != FALSE; }), "(the app window is minimized again)");
    t0 = GetTickCount64();
    Result up = Call(pipe, Command::Activate, W(app.main), self, 4000);
    ULONGLONG took = GetTickCount64() - t0;
    std::printf("  (Activate on the minimized window: %ls after %llu ms, front: %s)\n", ResultName(up), (unsigned long long)took,
                GetForegroundWindow() == app.main ? "the app window" : "another window");
    Check(up == Result::Ok && took < 1000 && WaitFor([&] { return !IsIconic(app.main); }), "a minimized window: Ok at once, restored");
    {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr), wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HWND stuck = nullptr;
        DWORD stuckThread = 0;
        std::thread t([&] {
            stuckThread = GetCurrentThreadId();
            WNDCLASSW wc = {};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"LiquidDockAdminTestStuck";
            RegisterClassW(&wc);
            stuck = CreateWindowExW(0, wc.lpszClassName, L"admin test app (not answering)", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_MINIMIZE,
                                    100, 100, 320, 200, nullptr, nullptr, wc.hInstance, nullptr);
            SetEvent(ready);
            WaitForSingleObject(wake, 10000);   // its messages unread meanwhile
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
            if (IsWindow(stuck)) DestroyWindow(stuck);
        });
        WaitForSingleObject(ready, 5000);
        Check(IsWindow(stuck) && IsIconic(stuck), "(a minimized window whose program reads no messages)");
        t0 = GetTickCount64();
        Result r = Call(pipe, Command::Activate, W(stuck), self, 4000);
        took = GetTickCount64() - t0;
        std::printf("  (Activate on it: %ls after %llu ms)\n", ResultName(r), (unsigned long long)took);
        Check(r == Result::Ok && took < 1000, "a program that does not answer: Ok at once, the helper not held up");
        Check(Call(pipe, Command::Hello, 0, self) == Result::Ok, "and the helper serves the next request");
        SetEvent(wake);
        Check(WaitFor([&] { return !IsIconic(stuck); }), "its program reading messages again: the window comes up");
        PostThreadMessageW(stuckThread, WM_QUIT, 0, 0);
        t.join();
        CloseHandle(ready);
        CloseHandle(wake);
    }

    // who talks to whom
    service.client = L"LiquidDock.exe";
    Check(Call(pipe, Command::Hello, 0, self) == Result::WrongClient, "the helper serves Liquid Dock only (here: another file name)");
    Check(WaitFor([&] { return said(L"refused a request from", L"admin_helper_test.exe"); }), "and says which program it refused");
    service.client = LD_ADMIN_CLIENT_EXE;
    Check(Call(pipe, Command::Hello, 0, L"C:\\Program Files\\LiquidDock\\LiquidDockSensors.exe") == Result::NotTrusted,
          "the dock talks only to the installed helper (here the test answers: not trusted)");

    // a raw client sending something else
    {
        HANDLE h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(pipe.c_str(), 2000))
            h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
        char junk[7] = "hello!";
        Reply raw{}, rep{};
        DWORD got = 0;
        bool ok = h != INVALID_HANDLE_VALUE && TransactNamedPipe(h, junk, sizeof(junk), &raw, sizeof(raw), &got, nullptr);
        Check(ok && ParseReply(&raw, got, rep) && rep.result == (uint32_t)Result::BadRequest, "another program's bytes: a bad request");
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    // a client that connects and says nothing holds the helper up for a second at most
    {
        HANDLE silent = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (silent == INVALID_HANDLE_VALUE && WaitNamedPipeW(pipe.c_str(), 2000))
            silent = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(silent != INVALID_HANDLE_VALUE, "(a silent client connected)");
        ULONGLONG t1 = GetTickCount64();
        Result r = Call(pipe, Command::Hello, 0, self, 4000);
        ULONGLONG took = GetTickCount64() - t1;
        Check(r == Result::Ok && took < 3000, "the next request is served after the silent one times out");
        std::printf("  (waited %llu ms behind the silent client)\n", (unsigned long long)took);
        if (silent != INVALID_HANDLE_VALUE) CloseHandle(silent);
    }

    // close
    int servedBefore = service.served;
    Check(Call(pipe, Command::Close, W(app.main), self) == Result::Ok, "Close: done");
    Check(WaitFor([&] { return !IsWindow(app.main); }), "Close closes the window (WM_CLOSE)");
    Check(WaitFor([&] { return service.served == servedBefore + 1; }) && service.last == Result::Ok, "one request, one answer");

    // integrity levels: the dock's own and same-level windows need no helper
    Check(!Elevated(app.tool), "a window of a program at the dock's level: the dock itself");
    Check(!Elevated(nullptr), "no window");
    Check(OwnIntegrity() >= 0x1000 || OwnIntegrity() == 0, "the dock's own level can be read (or is reported as unknown)");

    app.Stop();
    stop = true;
    server.join();
    service.Stop();
    Check(!PipeExists(pipe), "the pipe goes with the helper");
    return Summary("admin_helper_test");
}
