#pragma once
// Windows of programs that run as administrator, and the dock.
//
// The dock runs as the user: FluentFlyout puts its widget into the dock's window only then (main_instance.cpp,
// started as administrator). Windows keeps such a program from restoring, minimizing or closing the
// windows of programs that run as administrator (UIPI: ShowWindow fails with "access denied", posted
// messages are dropped). Explorer's taskbar may do it, an ordinary program may not.
//
// So the optional elevated helper (LiquidDockSensors.exe: installed once, with one UAC prompt, into the
// admin-only %ProgramFiles%\LiquidDock and started through its scheduled task without prompts) does
// exactly these three things for the dock: restore and bring to the front, minimize, close. Over a
// named pipe that only the same user can open, for Liquid Dock only (admin_service.h), on windows that
// have a taskbar button (the dock shows no others). The dock talks to it only when the program it
// answers from is the one in the admin-only folder (admin_client.h).
//
// This header is the part both sides share, free of Windows headers so it can be tested with any
// C++20 compiler (tests/admin_protocol_test.cpp).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <initializer_list>
#include <string>

namespace adminhelper {

constexpr uint32_t kMagic = 0x4E57444Cu;   // "LDWN"
constexpr uint32_t kVersion = 1;

enum class Command : uint32_t {
    Hello = 0,      // is a helper there (no window)
    Activate = 1,   // restore it when minimized and bring it to the front
    Minimize = 2,   // as the window's own minimize button (WM_SYSCOMMAND SC_MINIMIZE)
    Close = 3,      // as the dock's "close window" (WM_CLOSE)
};

enum class Result : uint32_t {
    Ok = 0,
    BadRequest,     // not a request of this protocol
    WrongClient,    // the caller is not Liquid Dock
    NoWindow,       // not a window (any more)
    NotAllowed,     // not a window with a taskbar button: the helper leaves it alone
    Failed,         // Windows refused
    // only on the dock's side
    NotRunning,     // no helper answers
    NotTrusted,     // the program answering on the helper's pipe is not the installed helper
    NoAnswer,       // the helper did not answer in time
};

// For the logs (write it with %ls: the same in MSVC's and MinGW's wide printf).
inline const wchar_t* ResultName(Result r) {
    switch (r) {
    case Result::Ok: return L"ok";
    case Result::BadRequest: return L"bad request";
    case Result::WrongClient: return L"not Liquid Dock";
    case Result::NoWindow: return L"no such window";
    case Result::NotAllowed: return L"not an app window";
    case Result::Failed: return L"refused by Windows";
    case Result::NotRunning: return L"helper not running";
    case Result::NotTrusted: return L"not the installed helper";
    case Result::NoAnswer: return L"no answer";
    }
    return L"?";
}

// Fixed-size messages, little-endian as both sides are the same machine. The window travels as a
// 64-bit number (a window handle is valid in every process of the session).
struct Request {
    uint32_t magic, version, command, reserved;
    uint64_t window;
};
struct Reply {
    uint32_t magic, version, result, reserved;
};
static_assert(sizeof(Request) == 24 && sizeof(Reply) == 16, "the pipe messages have fixed sizes");

// The installed helper's file version (VS_FIXEDFILEINFO: major.minor in the high half, build.revision
// in the low half). The window service came with 0.2.0-mod.10 (0,2,0,10): an older installed copy, or
// one without a version, answers no requests and is updated instead.
constexpr uint64_t kFirstServiceVersion = (0ull << 48) | (2ull << 32) | (0ull << 16) | 10ull;
inline uint64_t PackVersion(uint32_t fileVersionMS, uint32_t fileVersionLS) { return ((uint64_t)fileVersionMS << 32) | fileVersionLS; }
inline bool HelperCurrent(uint64_t installedVersion) { return installedVersion >= kFirstServiceVersion; }
// From 0.2.0-mod.24 the helper takes the right to change the front window itself, right after a click on the dock
// (admin_service.h BringToFront): an older one answers a request to bring a window up over an administrator's window
// and only flashes it when the dock had no right to pass on.
constexpr uint64_t kFirstFrontVersion = (0ull << 48) | (2ull << 32) | (0ull << 16) | 24ull;

// Whether the helper in the admin-only folder is (re)installed from the copy shipped with the dock
// (LiquidDockSensors.exe --install, one UAC prompt): none there, or not byte for byte the shipped one. By the
// bytes, not by the version: a rebuild of the same version counts too, and an identical copy asks nothing. With
// no shipped copy to install from, the installed one stays. Each: the whole file, or haveX false when there is
// no such file or it could not be read. Setup asks this only when a helper is installed (setup_main.cpp); the
// dock's switches also when none is (admin_helper.cpp HelperNeedsInstall).
inline bool HelperNeedsInstall(bool haveInstalled, const void* installed, size_t installedSize,
                               bool haveShipped, const void* shipped, size_t shippedSize) {
    if (!haveInstalled) return true;
    if (!haveShipped) return false;
    return installedSize != shippedSize || (installedSize && std::memcmp(installed, shipped, installedSize) != 0);
}

inline Request MakeRequest(Command c, uint64_t window) { return { kMagic, kVersion, (uint32_t)c, 0, window }; }
inline Reply MakeReply(Result r) { return { kMagic, kVersion, (uint32_t)r, 0 }; }

// What the helper acts on: exactly one Request of this protocol, a known command, a window for every
// command but Hello. A newer dock sends a higher version; the commands this helper knows stay valid.
inline bool ParseRequest(const void* data, size_t size, Request& out) {
    if (!data || size != sizeof(Request)) return false;
    std::memcpy(&out, data, sizeof(Request));
    if (out.magic != kMagic || out.version < 1) return false;
    if (out.command > (uint32_t)Command::Close) return false;
    if (out.command != (uint32_t)Command::Hello && out.window == 0) return false;
    return true;
}

inline bool ParseReply(const void* data, size_t size, Reply& out) {
    if (!data || size != sizeof(Reply)) return false;
    std::memcpy(&out, data, sizeof(Reply));
    return out.magic == kMagic && out.version >= 1 && out.result <= (uint32_t)Result::Failed;
}

// ---------------------------------------------------------------- which windows

// The windows the helper may touch: the ones that get a taskbar button, by the shell's rule (and the
// dock's, apps.cpp IsTaskbarWindow): a visible top-level window that is an app window, or unowned and
// neither a tool window nor a no-activate window; never the desktop or a taskbar.
struct WindowFacts {
    bool exists = false;
    bool visible = false;      // WS_VISIBLE (a minimized window is visible)
    bool topLevel = false;     // not a child window
    bool owned = false;        // has an owner window (dialogs, palettes)
    uint32_t exStyle = 0;
    std::wstring className;
};

constexpr uint32_t kExToolWindow = 0x00000080;   // WS_EX_TOOLWINDOW
constexpr uint32_t kExAppWindow = 0x00040000;    // WS_EX_APPWINDOW
constexpr uint32_t kExNoActivate = 0x08000000;   // WS_EX_NOACTIVATE

inline Result Eligible(const WindowFacts& w) {
    if (!w.exists) return Result::NoWindow;
    if (!w.visible || !w.topLevel) return Result::NotAllowed;
    if (!(w.exStyle & kExAppWindow) && (w.owned || (w.exStyle & (kExToolWindow | kExNoActivate)))) return Result::NotAllowed;
    for (const wchar_t* shell : { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" })
        if (w.className == shell) return Result::NotAllowed;
    return Result::Ok;
}

// ---------------------------------------------------------------- who is talking

// The pipe of one session (each user's dock and helper run in their own session).
inline std::wstring PipeName(uint32_t session, const std::wstring& prefix = L"\\\\.\\pipe\\LiquidDock.AdminHelper.") {
    return prefix + std::to_wstring(session);
}

inline std::wstring FileOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Windows file names and paths compare without case.
inline bool SameNoCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}

// The helper serves Liquid Dock only: the calling program's file name. (Defence in depth: any program
// of the same user could start a LiquidDock.exe; what the helper does stays limited to the three
// commands on taskbar windows.)
inline bool ClientAllowed(const std::wstring& clientImage, const std::wstring& expectedName) {
    return !clientImage.empty() && SameNoCase(FileOf(clientImage), expectedName);
}

// The dock talks only to the helper in the admin-only folder: a program there cannot have been put
// there by an ordinary program, and the helper refuses to run from anywhere else.
inline bool ServerTrusted(const std::wstring& serverImage, const std::wstring& expectedPath) {
    return !serverImage.empty() && !expectedPath.empty() && SameNoCase(serverImage, expectedPath);
}

// Integrity levels (the last part of the level's SID: 0x1000 low, 0x2000 medium, 0x3000 high,
// 0x4000 system): the dock needs the helper for a window whose program runs above the dock's own
// level, or whose level it could not read.
inline bool NeedsHelper(uint32_t targetLevel, uint32_t ownLevel, bool targetUnknown) {
    return targetUnknown || targetLevel > ownLevel;
}

}   // namespace adminhelper
