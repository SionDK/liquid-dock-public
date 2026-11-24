#include "tray_host_internal.h"
#include <shlwapi.h>   // SHAllocShared and the like (ForwardAppBar): shlwapi.lib on every link line that takes the tray host

namespace trayhost {

// host thread state (the other parts use these: tray_host_internal.h)
std::atomic<bool> g_ready{ false };
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
HWND g_wnd = nullptr, g_notifyWnd = nullptr;
std::atomic<bool> g_elevated{ false };    // this process runs as administrator (set when the host starts)
// ...and this file's alone
namespace {
std::atomic<bool> g_stop{ false };
bool g_gaveUp = false;                    // UI thread: Stop() gave up waiting for g_thread (kept until it has ended)
HANDLE g_readyEvent = nullptr;
HWND g_explorer = nullptr, g_lastExplorer = nullptr;
bool g_explorerMissed = false;            // a message did not reach Explorer: re-sync it when stopping
std::atomic<bool> g_announce{ true };     // ask programs to re-add their icons when starting (tray icons shown)
std::atomic<bool> g_announced{ false };   // ... asked in this run
}   // namespace

// ---------------------------------------------------------------- Explorer
HWND ExplorerTray() {
    if (!g_explorer || !IsWindow(g_explorer)) g_explorer = ExplorerTaskbar();
    return g_explorer;
}

// The one taskbar message that is handed on posted, not sent (WM_USER + 372; its meaning is not known here).
constexpr UINT kPostedTaskbarMsg = WM_USER + 372;

// Hands a message on to Explorer's taskbar and returns its answer.
bool Forward(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    result = 0;
    HWND ex = ExplorerTray();
    if (!ex) { g_explorerMissed = true; return false; }
    if (msg == kPostedTaskbarMsg) {   // posted by its senders (as ManagedShell forwards it)
        PostMessageW(ex, msg, wp, lp);
        return true;
    }
    DWORD_PTR r = 0;
    // bounded: the program waits for this answer (Shell_NotifyIcon); a hung Explorer fails at once
    if (!SendMessageTimeoutW(ex, msg, wp, lp, SMTO_ABORTIFHUNG, 2000, &r)) { g_explorerMissed = true; return false; }
    result = (LRESULT)r;
    return true;
}

// ---------------------------------------------------------------- app bars
// What SHAppBarMessage sends the first "Shell_TrayWnd" (WM_COPYDATA, dwData 0), as ManagedShell's APPBARMSGDATAV3: the
// APPBARDATA with 32-bit handles, the message and, for the ones with a rectangle (ABM_QUERYPOS, ABM_SETPOS,
// ABM_GETTASKBARPOS), a shared block (SHAllocShared) holding that APPBARDATA, made for the process of the window it
// found, whose number comes with it. The taskbar answers in that block.
struct AppBarData32 { DWORD cbSize; DWORD hWnd; UINT uCallbackMessage; UINT uEdge; RECT rc; INT64 lParam; };
struct AppBarMessage { AppBarData32 abd; DWORD dwMessage; DWORD pad; UINT64 hSharedMemory; DWORD dwProcessId; DWORD pad2; };
static_assert(sizeof(AppBarData32) == 40 && sizeof(AppBarMessage) == 64, "APPBARMSGDATAV3 layout");
static_assert(offsetof(AppBarMessage, dwMessage) == 40 && offsetof(AppBarMessage, hSharedMemory) == 48 &&
              offsetof(AppBarMessage, dwProcessId) == 56, "APPBARMSGDATAV3 layout");

// The block is made for the process of the window SHAppBarMessage found: the host's, so the dock's. A program's is a
// handle Explorer can take from the dock. But a block made for the process that makes it is no handle - Windows hands
// out a block in that process's own memory (2026-11-22, 26H2) - and the dock's own messages (its always-shown strip)
// find the host first, in the dock's process. Explorer, passed that, reads nothing: ABM_QUERYPOS answers nothing and
// ABM_SETPOS takes no strip, while both say TRUE, so maximised windows went under the dock until it was registered
// while Explorer's taskbar happened to be first. For the dock's own messages (OwnAppBar names the window while it
// sends) the block is copied into one made for Explorer's process, the message passed on with that, and Explorer's
// answer copied back. Everything else goes on as it came, as before: the host opens no block of another program's
// (an elevated dock would read and write it with rights the program does not have).
namespace { std::atomic<HWND> g_ownAppBar{ nullptr }; }
OwnAppBar::OwnAppBar(HWND w) { g_ownAppBar = w; }
OwnAppBar::~OwnAppBar() { g_ownAppBar = nullptr; }

#ifndef LD_TRAY_EXPLORER_PROCESS
#define LD_TRAY_EXPLORER_PROCESS(wnd) WindowProcess(wnd)
static DWORD WindowProcess(HWND w) { DWORD pid = 0; GetWindowThreadProcessId(w, &pid); return pid; }
#endif
bool ForwardAppBar(WPARAM wp, LPARAM lp, LRESULT& result) {
    const auto* cds = (const COPYDATASTRUCT*)lp;
    AppBarMessage m{};
    const HWND own = g_ownAppBar;
    if (!own || (HWND)wp != own || !cds || cds->cbData != sizeof(m) || !cds->lpData) return Forward(WM_COPYDATA, wp, lp, result);
    memcpy(&m, cds->lpData, sizeof(m));
    HWND ex = ExplorerTray();
    const DWORD exPid = ex ? LD_TRAY_EXPLORER_PROCESS(ex) : 0;
    if (!m.hSharedMemory || m.abd.cbSize != sizeof(AppBarData32) || m.abd.hWnd != (DWORD)(ULONG_PTR)own ||
        m.dwProcessId != GetCurrentProcessId() || !exPid)
        return Forward(WM_COPYDATA, wp, lp, result);
    void* block = SHLockShared((HANDLE)(ULONG_PTR)m.hSharedMemory, m.dwProcessId);
    if (!block) return Forward(WM_COPYDATA, wp, lp, result);
    HANDLE theirs = SHAllocShared(block, sizeof(AppBarData32), exPid);
    if (!theirs) { SHUnlockShared(block); return Forward(WM_COPYDATA, wp, lp, result); }
    AppBarMessage sent = m;
    sent.hSharedMemory = (UINT64)(ULONG_PTR)theirs;
    sent.dwProcessId = exPid;
    COPYDATASTRUCT out{ cds->dwData, sizeof(sent), &sent };
    const bool passed = Forward(WM_COPYDATA, wp, (LPARAM)&out, result);
    // Taken back only from the Explorer it was made for, once it has answered. Not answered (busy past the 2 s): the
    // message may still be waiting there and its block is left to it - closing it could close another of Explorer's
    // handles that takes the number. (Only the dock's own strip, so seldom; CheckAppBar asks for the strip again.)
    if (passed && ExplorerTray() == ex && LD_TRAY_EXPLORER_PROCESS(ex) == exPid) {
        if (void* answer = SHLockShared(theirs, exPid)) {
            memcpy(block, answer, sizeof(AppBarData32));
            SHUnlockShared(answer);
        }
        SHFreeShared(theirs, exPid);
    }
    SHUnlockShared(block);
    return passed;
}

namespace {

// The window's own pixels are see-through while it keeps a surface. A hosted widget needs that surface:
// FluentFlyout's is a WPF window with a transparent background - a layered window drawn with
// UpdateLayeredWindow - and as a child window it shows only in a parent with a redirection bitmap (in a
// window created with WS_EX_NOREDIRECTIONBITMAP it stays invisible, though placed and clickable). See-
// through: from Windows 8 the compositor blends a window with "blur behind" over an empty region by the
// alpha of what it draws, without blurring (GLFW's updateFramebufferTransparency), and this window
// draws black, alpha 0 (WM_ERASEBKGND) - so only the widget shows on the dock's glass.
void SeeThrough(HWND w) {
    HRGN none = CreateRectRgn(0, 0, -1, -1);
    DWM_BLURBEHIND bb{};
    bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    bb.fEnable = TRUE;
    bb.hRgnBlur = none;
    HRESULT hr = DwmEnableBlurBehindWindow(w, &bb);
    DeleteObject(none);
    if (FAILED(hr)) Log(L"tray host: no see-through window (0x%08lx): a hosted widget would show on black", (unsigned long)hr);
}

LRESULT CALLBACK HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COPYDATA:
        return OnCopyData(wp, lp);
    case WM_WINDOWPOSCHANGING:   // shown only by the dock, whoever else asks ("hide/show the taskbar" tools)
        if (lp && !g_hostedWidget.showing) ((WINDOWPOS*)lp)->flags &= ~SWP_SHOWWINDOW;
        break;
    case WM_WINDOWPOSCHANGED:
        if (lp && (((WINDOWPOS*)lp)->flags & SWP_SHOWWINDOW) && !g_hostedWidget.showing)
            SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) & ~WS_VISIBLE);
        break;
    case WM_ERASEBKGND: {   // black, alpha 0: see-through (SeeThrough), under the hosted widget too
        RECT rc{};
        GetClientRect(h, &rc);
        FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_MOUSEACTIVATE:   // a click on the hosted widget leaves the focus where it was, as on the dock
        return MA_NOACTIVATE;
    case WM_DWMCOMPOSITIONCHANGED:   // the compositor restarted: see-through again
        SeeThrough(h);
        break;
    case WM_CLOSE:
        return 0;   // only Stop() takes it down
    }
    // what programs send the taskbar (WM_COMMAND: minimize all, show desktop, ...; private WM_USER
    // messages) is Explorer's to answer. Registered messages (0xC000+) are broadcasts it gets itself.
    if (msg == WM_COMMAND || (msg >= WM_USER && msg < 0xC000)) {
        LRESULT r = 0;
        Forward(msg, wp, lp, r);
        return r;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void BroadcastTaskbarCreated() {
    // every program re-adds its icons (as when Explorer starts): this time they reach the host first
    UINT msg = RegisterWindowMessageW(LD_TRAY_CREATED_MESSAGE);
    if (!msg) return;
    SendNotifyMessageW(HWND_BROADCAST, msg, 0, 0);
    // a broadcast only reaches top-level windows: programs that keep their tray icon on a
    // message-only window are told directly (programs that do not know the message ignore it)
    int sent = 0;
    for (HWND w = nullptr; sent < 4000 && (w = FindWindowExW(HWND_MESSAGE, w, nullptr, nullptr)) != nullptr; ++sent)
        SendNotifyMessageW(w, msg, 0, 0);
}

// Programs find the tray with FindWindow("Shell_TrayWnd"): the first in z-order. Explorer's comes
// up again whenever its taskbar is raised, so the host goes back on top - within a tenth of a second
// (as ManagedShell). A message Explorer gets meanwhile is not lost: only the dock misses that update.
void KeepFirst() {
    if (FindWindowW(LD_TRAY_CLASS, nullptr) != g_wnd)
        SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
}

// host thread: icons of programs that died without removing them (Explorer drops those too)
void Prune() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (size_t i = g_icons.size(); i-- > 0;)
            if (!IsWindow(g_icons[i].hwnd)) { g_icons.erase(g_icons.begin() + i); changed = true; }
    }
    for (size_t i = g_exeCache.size(); i-- > 0;)
        if (!IsWindow(g_exeCache[i].first)) g_exeCache.erase(g_exeCache.begin() + i);
    if (changed) g_version++;
}

struct RegWatch {
    HKEY key = nullptr;
    HANDLE event = nullptr;
    bool onSettings = false;
    void Arm() {
        if (!event) event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (key && !onSettings) {   // watching "Control Panel" until NotifyIconSettings exists
                HKEY s = nullptr;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_NOTIFY | KEY_READ, &s) == ERROR_SUCCESS) {
                    RegCloseKey(key); key = s; onSettings = true;
                }
            }
            if (!key) {
                if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_NOTIFY | KEY_READ, &key) == ERROR_SUCCESS) onSettings = true;
                else if (RegOpenKeyExW(HKEY_CURRENT_USER, LD_TRAY_SETTINGS_PARENT, 0, KEY_NOTIFY, &key) != ERROR_SUCCESS) key = nullptr;
            }
            if (!key || !event) return;
            if (RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, event, TRUE) == ERROR_SUCCESS) return;
            RegCloseKey(key); key = nullptr; onSettings = false;   // deleted under us: watch from the parent again
        }
    }
    void Close() {
        if (key) RegCloseKey(key);
        if (event) CloseHandle(event);
        key = nullptr; event = nullptr; onSettings = false;
    }
};

// The host's windows, made on the host thread (COM initialised for it). False when it cannot run: Start()
// has been told (g_readyEvent) and COM left again, the thread only returns.
bool SetUpWindows(HWND& ex) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = HostProc; wc.hInstance = inst; wc.lpszClassName = LD_TRAY_CLASS;
    RegisterClassExW(&wc);   // fails harmlessly when a previous run registered it
    WNDCLASSEXW wn = { sizeof(wn) };
    wn.lpfnWndProc = DefWindowProcW; wn.hInstance = inst; wn.lpszClassName = L"TrayNotifyWnd";
    RegisterClassExW(&wn);
    g_explorer = nullptr;
    ex = ExplorerTray();
    RECT r{};
    if (ex) GetWindowRect(ex, &r);
    // Not WS_CLIPCHILDREN: it paints itself see-through under its children too (a layered child is
    // blended over what is under it). WS_EX_NOACTIVATE: shown over the dock, it takes no activation.
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, LD_TRAY_CLASS, L"", WS_POPUP | WS_CLIPSIBLINGS,
                            r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) {
        Log(L"tray host: window failed %lu", GetLastError());
        SetEvent(g_readyEvent);
        CoUninitialize();
        return false;
    }
    SeeThrough(g_wnd);
    SetPropW(g_wnd, kTrayHostProp, (HANDLE)1);
    // Explorer's taskbar never runs as administrator, so every program can hand it its icons. Started as
    // administrator, the dock would refuse the messages of ordinary programs (UIPI blocks WM_COPYDATA
    // from a lower integrity level) and, being first in line, cut them off from Explorer as well. Not
    // elevated it takes what Explorer takes: opened then, the filters would only let low-integrity
    // (sandboxed) processes in, whose messages it passes on with its own rights - Explorer lets neither
    // through from them (2026-10-28: no filter for WM_COMMAND or WM_COPYDATA in explorer.exe,
    // Taskbar.dll, twinui.dll or shell32.dll).
    g_elevated = LD_TRAY_ELEVATED;
    if (g_elevated) ChangeWindowMessageFilterEx(g_wnd, WM_COMMAND, MSGFLT_ALLOW, nullptr);   // passed on to Explorer (Win+M tools)
    if (g_elevated && !ChangeWindowMessageFilterEx(g_wnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr)) {
        Log(L"tray host: cannot take messages from programs that are not elevated (%lu): not started", GetLastError());
        RemovePropW(g_wnd, kTrayHostProp);
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
        SetEvent(g_readyEvent);
        CoUninitialize();
        return false;
    }
    g_notifyWnd = CreateWindowExW(0, L"TrayNotifyWnd", nullptr, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                  0, 0, r.right - r.left, r.bottom - r.top, g_wnd, nullptr, inst, nullptr);
    // a hosted widget sits above the dock's glass: kept out of the dock's backdrop capture (and out of
    // screenshots, like the dock), else the glass would show a blurred copy of it
    SetWindowDisplayAffinity(g_wnd, WDA_EXCLUDEFROMCAPTURE);
    return true;
}

// How many programs answered the re-registration, and which programs' icons have not come.
void LogSummary() {
    int icons = 0, shown = 0;
    std::vector<std::wstring> programs;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        icons = (int)g_icons.size();
        for (const auto& ic : g_icons) {
            if (ShownOnDock(ic)) ++shown;
            if (std::find(programs.begin(), programs.end(), ic.exe) == programs.end()) programs.push_back(ic.exe);
        }
    }
    Log(L"tray host: %d icons from %d programs so far, %d shown on the dock", icons, (int)programs.size(), shown);
    LogMissing(programs);
}

// The host thread ends (stopped, or WM_QUIT): its windows go, a hosted widget back to Explorer's taskbar.
void TearDown(RegWatch& watch) {
    g_ready = false;   // stopped, or ended on its own (WM_QUIT): not running any more, Start() makes a new one
    bool resync = g_explorerMissed;
    watch.Close();
    // Another program's widget goes back to Explorer's taskbar rather than being destroyed with this
    // window (it would recreate it, a second later) - nothing puts it back in place here after that.
    StopRealign();
    HWND back = ExplorerTray();
    for (HWND c = GetWindow(g_wnd, GW_CHILD); c;) {
        HWND next = GetWindow(c, GW_HWNDNEXT);
        if (c != g_notifyWnd && back) SetParent(c, back);
        c = next;
    }
    RemovePropW(g_wnd, kTrayHostProp);
    if (g_notifyWnd) DestroyWindow(g_notifyWnd);
    DestroyWindow(g_wnd);
    g_wnd = g_notifyWnd = nullptr;
    EndHosting();
    // Explorer missed some icon messages: let every program register its icons with it again
    if (resync) BroadcastTaskbarCreated();
    Log(L"tray host: down%s", resync ? L" (icons re-registered with Explorer)" : L"");
}

DWORD WINAPI HostThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // known folders
    HWND ex = nullptr;
    if (!SetUpWindows(ex)) return 1;
    KeepFirst();
    g_lastExplorer = ex;
    g_explorerMissed = false;
    g_hostedWidget.hostingApplied = false;
    LoadEntries();
    RegWatch watch;
    watch.Arm();
    g_ready = true;
    SetEvent(g_readyEvent);
    Log(L"tray host: up (explorer %p, %d settings entries%s%s)", (void*)ex, (int)g_entries.size(),
        g_elevated ? L", the dock runs as administrator" : L"", g_announce ? L"" : L", icons not requested");
    double nextCheck = 0, nextPrune = NowSeconds() + 2, reloadAt = 0, lastReload = NowSeconds(), rebroadcastAt = 0;
    double summaryAt = 0;   // how many programs answered the re-registration
    auto announce = [&] {
        g_announced = true;
        BroadcastTaskbarCreated();
        summaryAt = NowSeconds() + 5;
    };
    if (g_announce) announce();
    ApplyHosting();

    while (!g_stop) {
        DWORD handles = watch.event ? 1 : 0;
        DWORD wr = MsgWaitForMultipleObjectsEx(handles, handles ? &watch.event : nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) g_stop = true;
            if (!m.hwnd && m.message == kPlaceMsg) {   // from the dock
                if (m.wParam == kPlaceAnnounce) { if (!g_announced) announce(); }
                else { ApplyHosting(); ApplyPlacement(); if (g_hostedWidget.hostingApplied) UpdateHosted(); }
                continue;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        double now = NowSeconds();
        if (handles && wr == WAIT_OBJECT_0) {
            // icons being added write their entries, some programs keep changing theirs: at most once a second
            reloadAt = std::max(now + 0.3, lastReload + 1.0);
            watch.Arm();
        }
        if (reloadAt == 0 && now - lastReload > 30) reloadAt = now;   // in case a change notification was missed
        if (reloadAt > 0 && now >= reloadAt) {
            reloadAt = 0; lastReload = now;
            LoadEntries();
            if (UpdateAllPromotion()) g_version++;
        }
        if (now >= nextCheck) {
            nextCheck = now + 0.1;
            KeepFirst();
            ApplyHosting();
            if (g_hostedWidget.hostingApplied) {
                ApplyPlacement();   // also undoes "hide the taskbar" tools
                UpdateHosted();     // the hosted widget changes its region on its own (no message for that)
            } else MirrorRect();
            HWND cur = ExplorerTray();
            if (cur && cur != g_lastExplorer) {   // Explorer (re)started: its taskbar may have come up first
                if (g_announced && (g_lastExplorer || g_explorerMissed)) rebroadcastAt = now + 2.0;
                Log(L"tray host: explorer taskbar %p (was %p)", (void*)cur, (void*)g_lastExplorer);
                g_lastExplorer = cur;
            }
        }
        if (rebroadcastAt > 0 && now >= rebroadcastAt) {
            rebroadcastAt = 0;
            KeepFirst();
            g_explorerMissed = false;
            BroadcastTaskbarCreated();
        }
        if (now >= nextPrune) { nextPrune = now + 2; Prune(); }
        if (summaryAt > 0 && now >= summaryAt) {
            summaryAt = 0;
            LogSummary();
        }
    }

    TearDown(watch);
    CoUninitialize();
    return 0;
}

// UI thread: the host thread, once it has ended - its handle closed, what it knew of the icons dropped.
// False while it runs (also one Stop() gave up waiting for).
bool Reap() {
    if (!g_thread || WaitForSingleObject(g_thread, 0) != WAIT_OBJECT_0) return false;
    CloseHandle(g_thread);
    g_thread = nullptr; g_threadId = 0;
    g_gaveUp = false;
    g_exeCache.clear();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_icons.clear();
    }
    g_version++;
    return true;
}

}   // namespace

bool Start(bool announce) {
    Reap();   // a host thread that has ended since: one Stop() gave up waiting for, or one that ended on its own
    if (g_thread) {   // runs - or the one Stop() gave up on still does: never a second host next to it
        if (g_gaveUp) Log(L"tray host: not started, the last one has not stopped yet");
        if (announce) Announce();
        return g_ready;
    }
    g_announce = announce;
    g_announced = false;
    if (!g_readyEvent) g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_readyEvent) return false;
    ResetEvent(g_readyEvent);
    g_stop = false;
    g_ready = false;
    g_thread = CreateThread(nullptr, 0, HostThread, nullptr, 0, &g_threadId);
    if (!g_thread) return false;
    HANDLE wait[] = { g_readyEvent, g_thread };
    WaitForMultipleObjects(2, wait, FALSE, INFINITE);   // window set up (or failed): a few milliseconds
    if (!g_ready) { Stop(); return false; }
    return true;
}

void Stop() {
    if (Reap() || !g_thread) return;   // (ended already: cleaned up now)
    if (g_gaveUp) return;   // waited for once already (10 s and more): cleaned up once it has ended, by Start() or Stop()
    g_ready = false;
    g_stop = true;
    PostThreadMessageW(g_threadId, WM_NULL, 0, 0);
    // Keep answering messages sent to this thread while waiting: the host may be passing one on to
    // Explorer, which can be sending to a dock window here at the same time.
    for (int i = 0; i < 400; ++i) {
        DWORD r = MsgWaitForMultipleObjects(1, &g_thread, FALSE, 25, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0) break;
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    if (!Reap()) {   // its last step (a hosted widget back to Explorer) waits on that widget's program
        g_gaveUp = true;
        Log(L"tray host: did not stop in time");
        return;   // kept: Start() will not run a second host next to it, and cleans up once it has ended
    }
}

bool Running() { return g_thread && g_ready; }

void Announce() {
    if (g_thread && g_ready && !g_announced) PostThreadMessageW(g_threadId, kPlaceMsg, kPlaceAnnounce, 0);
}

HWND Window() { return g_thread && g_ready ? g_wnd : nullptr; }

}   // namespace trayhost
