#pragma once
// The dock's side of the admin window service (admin_protocol.h): which windows need the helper, and
// one request per connection with every wait bounded. Header-only so the tests can use it without the
// rest of the dock (admin_helper.cpp has the dock's policy: settings, the offer to install, the queue).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include "admin_protocol.h"

namespace adminhelper {

// The integrity level of a token (the last part of its level's SID), 0 when it cannot be read.
inline DWORD IntegrityOf(HANDLE token) {
    DWORD n = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &n);
    if (!n) return 0;
    std::vector<BYTE> buf(n);
    if (!GetTokenInformation(token, TokenIntegrityLevel, buf.data(), n, &n)) return 0;
    PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data())->Label.Sid;
    UCHAR count = *GetSidSubAuthorityCount(sid);
    return count ? *GetSidSubAuthority(sid, count - 1) : 0;
}

inline DWORD OwnIntegrity() {
    static const DWORD level = [] {
        HANDLE t = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return (DWORD)0;
        DWORD l = IntegrityOf(t);
        CloseHandle(t);
        return l;
    }();
    return level;
}

// The window's program runs above the dock's integrity level (as administrator): Windows keeps the
// dock from restoring, minimizing or closing its windows.
inline bool Elevated(HWND h) {
    DWORD pid = 0;
    if (!h || !GetWindowThreadProcessId(h, &pid) || !pid || pid == GetCurrentProcessId()) return false;
    if (!OwnIntegrity()) return false;   // cannot compare: the dock's own calls, as before
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;   // gone, or a protected process: the dock's own calls, as before
    HANDLE t = nullptr;
    bool unknown = !OpenProcessToken(p, TOKEN_QUERY, &t);
    DWORD level = unknown ? 0 : IntegrityOf(t);
    // a UIAccess program (accessibility tools) is out of reach the same way, at the dock's own level
    DWORD uiAccess = 0, n = 0;
    if (t && !GetTokenInformation(t, TokenUIAccess, &uiAccess, sizeof(uiAccess), &n)) uiAccess = 0;
    if (t) CloseHandle(t);
    CloseHandle(p);
    return NeedsHelper(level, OwnIntegrity(), unknown || level == 0) || uiAccess != 0;
}

inline std::wstring ImageOfProcess(DWORD pid) {
    std::wstring out;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2];
        DWORD n = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(p, 0, path, &n)) out.assign(path, n);
        CloseHandle(p);
    }
    return out;
}

// A helper's pipe is there (without connecting to it).
inline bool PipeExists(const std::wstring& pipe) {
    if (WaitNamedPipeW(pipe.c_str(), 1)) return true;
    return GetLastError() == ERROR_SEM_TIMEOUT;   // there, busy with another client
}

// One request. trustedServer: the full path of the program that must be answering (the installed
// helper); Activate lets that program bring a window to the front (the dock may: it has just been
// clicked). waitMs: how long to wait for a helper busy with another client.
inline Result Call(const std::wstring& pipe, Command c, uint64_t window, const std::wstring& trustedServer, DWORD waitMs = 1500) {
    HANDLE h = INVALID_HANDLE_VALUE;
    const ULONGLONG until = GetTickCount64() + waitMs;
    for (;;) {
        h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return Result::NotRunning;
        ULONGLONG now = GetTickCount64();
        if (now >= until) return Result::NoAnswer;
        if (!WaitNamedPipeW(pipe.c_str(), (DWORD)(until - now)) && GetLastError() == ERROR_FILE_NOT_FOUND) return Result::NotRunning;
    }
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(h, &pid) || !ServerTrusted(ImageOfProcess(pid), trustedServer)) {
        CloseHandle(h);
        return Result::NotTrusted;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
    if (c == Command::Activate) AllowSetForegroundWindow(pid);
    Request req = MakeRequest(c, window);
    Reply raw{}, reply{};
    Result r = Result::NoAnswer;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD got = 0;
    if (ov.hEvent && (TransactNamedPipe(h, &req, sizeof(req), &raw, sizeof(raw), nullptr, &ov) || GetLastError() == ERROR_IO_PENDING)) {
        if (WaitForSingleObject(ov.hEvent, 1500) == WAIT_OBJECT_0 && GetOverlappedResult(h, &ov, &got, FALSE))
            r = ParseReply(&raw, got, reply) ? (Result)reply.result : Result::NoAnswer;
        else {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, &got, TRUE);
        }
    }
    if (ov.hEvent) CloseHandle(ov.hEvent);
    CloseHandle(h);
    return r;
}

}   // namespace adminhelper
