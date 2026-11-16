#pragma once
// The elevated helper's side of the admin window service (admin_protocol.h): one named pipe, one
// client at a time. The pipe opens to the same user only (any integrity level from medium up), never
// over the network; the caller must be LiquidDock.exe of this session. Every wait is bounded, so a
// program that connects and stays silent cannot hold the helper up for more than a second.
// Header-only: the helper (sensors/sensors_main.cpp) and the tests (tests/admin_helper_test.cpp)
// include it; the dock itself does not.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <cwchar>
#include <string>
#include <vector>
#include "admin_protocol.h"

#ifndef LD_ADMIN_CLIENT_EXE
#define LD_ADMIN_CLIENT_EXE L"LiquidDock.exe"
#endif

namespace adminhelper {

class Service {
public:
    using LogFn = void (*)(const wchar_t* line);

    ~Service() { Stop(); }

    // Creates the pipe (first instance only: if another program already holds the name, fail).
    bool Start(const std::wstring& name, LogFn log = nullptr) {
        Stop();
        log_ = log;
        name_ = name;
        connected_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        io_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!connected_ || !io_) return false;
        std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;" + UserSid() + L")";
        PSECURITY_DESCRIPTOR sd = nullptr;
        // medium label, no write up and no read up: the (non-elevated) dock may use the pipe,
        // low-integrity programs (sandboxed browsers and the like) may not even open it - opened to read
        // only, it would still take the helper's one connection and keep the dock waiting
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW((sddl + L"S:(ML;;NWNR;;;ME)").c_str(), SDDL_REVISION_1, &sd, nullptr) &&
            !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
            sd = nullptr;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
        pipe_ = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                 1, 256, 256, 0, sd ? &sa : nullptr);
        DWORD err = GetLastError();
        if (sd) LocalFree(sd);
        if (pipe_ == INVALID_HANDLE_VALUE) {
            Say(L"window service: pipe not created (%lu)", (unsigned long)err);
            return false;
        }
        return Listen();
    }

    // Waits up to about timeoutMs for a client and serves it (one request).
    void Poll(DWORD timeoutMs) {
        if (pipe_ == INVALID_HANDLE_VALUE) { Sleep(timeoutMs); return; }
        // a pipe that did not listen is tried again within a second, however long the caller would wait
        // (the dock waits 1.5 s for a busy pipe)
        if (!listening_ && !Listen()) { Sleep(timeoutMs < 1000 ? timeoutMs : 1000); return; }
        if (WaitForSingleObject(connected_, timeoutMs) != WAIT_OBJECT_0) return;
        listening_ = false;
        Serve();
        DisconnectNamedPipe(pipe_);
        Listen();
    }

    void Stop() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe_, nullptr);
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        if (connected_) { CloseHandle(connected_); connected_ = nullptr; }
        if (io_) { CloseHandle(io_); io_ = nullptr; }
        listening_ = false;
    }

    std::wstring client = LD_ADMIN_CLIENT_EXE;   // the program file name the helper serves
    std::atomic<int> served{ 0 };                // requests answered (for the tests)
    std::atomic<Result> last{ Result::Ok };      // the last answer (for the tests)

private:
    // Wide strings as %ls (MSVC's and MinGW's wide printf read %s differently).
    template <class... A> void Say(const wchar_t* fmt, A... a) {
        if (!log_) return;
        wchar_t line[512];
        swprintf(line, 512, fmt, a...);
        log_(line);
    }

    static std::wstring UserSid() {
        std::wstring out;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return out;
        DWORD n = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &n);
        std::vector<BYTE> buf(n ? n : 1);
        LPWSTR s = nullptr;
        if (n && GetTokenInformation(token, TokenUser, buf.data(), n, &n) &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
            out = s;
            LocalFree(s);
        }
        CloseHandle(token);
        return out;
    }

    bool Listen() {
        ZeroMemory(&connect_, sizeof(connect_));
        connect_.hEvent = connected_;
        ResetEvent(connected_);
        BOOL ok = ConnectNamedPipe(pipe_, &connect_);
        if (!ok && GetLastError() == ERROR_NO_DATA) {
            // a client opened the new pipe and closed it again before this listened: the pipe stays
            // closing, open to no one, until it is disconnected. Once is enough: a disconnected pipe
            // takes no client before it listens again.
            Say(L"window service: a client came and went before it listened");
            DisconnectNamedPipe(pipe_);
            ok = ConnectNamedPipe(pipe_, &connect_);
        }
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) SetEvent(connected_);
            else if (e != ERROR_IO_PENDING) return false;
        }
        listening_ = true;
        return true;
    }

    // One overlapped read or write, at most `ms` long.
    bool Io(bool write, void* buf, DWORD size, DWORD ms, DWORD& done) {
        OVERLAPPED ov = {};
        ov.hEvent = io_;
        ResetEvent(io_);
        done = 0;
        BOOL ok = write ? WriteFile(pipe_, buf, size, nullptr, &ov) : ReadFile(pipe_, buf, size, nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(io_, ms) != WAIT_OBJECT_0) {
            CancelIoEx(pipe_, &ov);
            GetOverlappedResult(pipe_, &ov, &done, TRUE);
            return false;
        }
        return GetOverlappedResult(pipe_, &ov, &done, FALSE) != FALSE;
    }

    static std::wstring ImageOf(DWORD pid) {
        std::wstring out;
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH * 2];
            DWORD n = ARRAYSIZE(path);
            if (QueryFullProcessImageNameW(p, 0, path, &n)) out.assign(path, n);
            CloseHandle(p);
        }
        return out;
    }

    bool ClientOk(std::wstring& image, DWORD& clientPid) {
        ULONG pid = 0;
        if (!GetNamedPipeClientProcessId(pipe_, &pid) || !pid) return false;
        clientPid = pid;
        DWORD theirs = 0, ours = 0;
        if (!ProcessIdToSessionId(pid, &theirs) || !ProcessIdToSessionId(GetCurrentProcessId(), &ours) || theirs != ours) return false;
        image = ImageOf(pid);
        return ClientAllowed(image, client);
    }

    static WindowFacts Facts(HWND h) {
        WindowFacts f;
        f.exists = IsWindow(h) != FALSE;
        if (!f.exists) return f;
        f.visible = IsWindowVisible(h) != FALSE;
        f.topLevel = GetAncestor(h, GA_ROOT) == h;
        f.owned = GetWindow(h, GW_OWNER) != nullptr;
        f.exStyle = (uint32_t)GetWindowLongPtrW(h, GWL_EXSTYLE);
        wchar_t cls[128] = {};
        GetClassNameW(h, cls, ARRAYSIZE(cls));
        f.className = cls;
        return f;
    }

    // The pointer is on a window of the client (the dock: its icons, previews, window list) and there was input a
    // moment ago: a click on the dock, not a program of the user's that only calls itself LiquidDock.exe.
    static bool ClickedOnClient(DWORD clientPid) {
        LASTINPUTINFO li = { sizeof(li) };
        if (!clientPid || !GetLastInputInfo(&li) || GetTickCount() - li.dwTime > 2000) return false;
        POINT pt{};
        if (!GetCursorPos(&pt)) return false;
        const HWND under = WindowFromPoint(pt);
        DWORD pid = 0;
        if (under) GetWindowThreadProcessId(GetAncestor(under, GA_ROOT), &pid);
        return pid == clientPid;
    }

    // The window put in front. The dock allowed this process to (AllowSetForegroundWindow) - when it had the
    // right itself: its key for the right does not reach a window that runs as administrator in front, and that
    // is when it asks for an ordinary window too (adminhelper::ActivateOver). So without it, and right after a
    // click on the dock only, the helper's own input: the same unassigned key, which at this integrity level
    // reaches that window; the right comes once Windows has taken the key in (looked for up to 0.1 s). Never
    // attaching to the thread in front: with the input queues shared, the change of front waits on that program.
    static void BringToFront(HWND h, DWORD clientPid) {
        if (SetForegroundWindow(h) && GetForegroundWindow() == h) return;
        if (!ClickedOnClient(clientPid)) return;
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = 0xE8;   // "unassigned": no program acts on it (system.h TakeForegroundRight)
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(2, in, sizeof(INPUT)) != 2) return;
        for (int i = 0; i < 10; ++i) {
            if (SetForegroundWindow(h) && GetForegroundWindow() == h) return;
            Sleep(10);
        }
    }

    // Only calls that cannot block on a hung program: ShowWindowAsync, SetForegroundWindow, PostMessage.
    static Result Execute(const Request& r, DWORD clientPid = 0) {
        if (r.command == (uint32_t)Command::Hello) return Result::Ok;
        HWND h = reinterpret_cast<HWND>(static_cast<uintptr_t>(r.window));
        Result e = Eligible(Facts(h));
        if (e != Result::Ok) return e;
        switch ((Command)r.command) {
        case Command::Activate:
            if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
            BringToFront(h, clientPid);
            return Result::Ok;
        case Command::Minimize:
            return PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0) ? Result::Ok : Result::Failed;
        case Command::Close:
            return PostMessageW(h, WM_CLOSE, 0, 0) ? Result::Ok : Result::Failed;
        default:
            return Result::BadRequest;
        }
    }

    void Serve() {
        unsigned char buf[256];
        DWORD got = 0;
        std::wstring image;
        DWORD clientPid = 0;
        bool clientOk = ClientOk(image, clientPid);
        if (!Io(false, buf, sizeof(buf), 1000, got)) return;   // silent, gone or too long: next client
        Request req{};
        Result r = !clientOk ? Result::WrongClient : !ParseRequest(buf, got, req) ? Result::BadRequest : Execute(req, clientPid);
        Reply reply = MakeReply(r);
        DWORD sent = 0;
        last = r;   // counted before the reply goes out: a client that has its answer sees it counted
        ++served;
        bool replied = Io(true, &reply, sizeof(reply), 1000, sent);
        if (r == Result::WrongClient) Say(L"window service: refused a request from %ls", image.empty() ? L"(unknown program)" : image.c_str());
        else if (r == Result::BadRequest) Say(L"window service: a request it does not understand (%lu bytes)", (unsigned long)got);
        else if (r != Result::Ok && r != Result::NoWindow) Say(L"window service: command %u: %ls", (unsigned)req.command, ResultName(r));
        if (replied) Io(false, buf, sizeof(buf), 1000, got);   // until the client has read it and closed: disconnecting
                                                              // earlier would throw an unread reply away
    }

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE connected_ = nullptr, io_ = nullptr;
    OVERLAPPED connect_ = {};
    bool listening_ = false;
    std::wstring name_;
    LogFn log_ = nullptr;
};

}   // namespace adminhelper
