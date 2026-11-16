// The OLED mode (oled*.cpp) with an in-memory desktop: monitors, Explorer's wallpaper API (IDesktopWallpaper),
// the desktop windows and a z-order. No real window is made, moved or covered, no wallpaper, desktop icon or
// other program is touched: every Windows call that would reach them is replaced below. Its record (oled.ini)
// and black picture go to preview-oled-data next to this test's exe, and are deleted at the end.
// One PASS/FAIL line per check; the exit code is the number of failures.
#include "../src/common.h"
#include "../src/oled.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shlguid.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <fstream>
#include <map>
#include <cstdio>
#include <cstdarg>
#include "portable_check.h"

// ---- what common.cpp gives the dock: a clock this test moves, its log, its data folder
static double g_now = 1000;
double NowSeconds() { return g_now; }
static std::vector<std::wstring> logs;
void Log(const wchar_t* fmt, ...) {
    wchar_t line[1024]; va_list ap; va_start(ap, fmt); _vsnwprintf_s(line, _TRUNCATE, fmt, ap); va_end(ap);
    logs.push_back(line);
}
std::wstring AppDataDir() {
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        dir = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\')) + L"\\preview-oled-data";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}
std::wstring ToLower(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size()); return s; }
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr); return s;
}
std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n); return w;
}
// as common.cpp: to <path>.tmp, flushed, swapped in; the old file stays when either step fails
bool SaveFileWhole(const std::wstring& path, const std::string& text, const wchar_t* what) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) &&
                    wrote == text.size() && FlushFileBuffers(f);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) { DeleteFileW(tmp.c_str()); Log(L"%s: could not be written", what); return false; }
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    Log(L"%s: could not replace", what);
    return false;
}

// ---- monitors, as the wallpaper API and the window manager see them
struct Monitor { HMONITOR handle; RECT rect; std::wstring id; bool attached = true; std::wstring wallpaper; };
static std::vector<Monitor> monitors;
static Monitor* ById(LPCWSTR id) {
    for (auto& m : monitors) if (id && m.id == id) return &m;
    return nullptr;
}
static BOOL WINAPI FakeEnumMonitors(HDC, LPCRECT, MONITORENUMPROC proc, LPARAM lp) {
    for (auto& m : monitors) if (m.attached && !proc(m.handle, nullptr, &m.rect, lp)) break;
    return TRUE;
}
static BOOL WINAPI FakeMonitorInfo(HMONITOR h, LPMONITORINFO mi) {
    for (auto& m : monitors) if (m.attached && m.handle == h) { mi->rcMonitor = mi->rcWork = m.rect; return TRUE; }
    return FALSE;
}
static HMONITOR WINAPI FakeMonitorFromRect(LPCRECT r, DWORD flags) {
    HMONITOR best = nullptr; LONG area = 0;
    for (auto& m : monitors) {
        RECT both{};
        if (m.attached && IntersectRect(&both, r, &m.rect) && (both.right - both.left) * (both.bottom - both.top) > area) {
            area = (both.right - both.left) * (both.bottom - both.top); best = m.handle;
        }
    }
    if (!best && flags != MONITOR_DEFAULTTONULL) for (auto& m : monitors) if (m.attached) return m.handle;
    return best;
}
static int WINAPI FakeMetrics(int index) {
    RECT all{};
    for (auto& m : monitors) if (m.attached) UnionRect(&all, &all, &m.rect);
    switch (index) {
    case SM_XVIRTUALSCREEN: return all.left;
    case SM_YVIRTUALSCREEN: return all.top;
    case SM_CXVIRTUALSCREEN: return all.right - all.left;
    case SM_CYVIRTUALSCREEN: return all.bottom - all.top;
    }
    return 0;
}

// ---- Explorer's wallpaper API (IDesktopWallpaper): per monitor, by device path
static bool wallpaperApi = true;   // false: Explorer is not running (CoCreateInstance fails)
static COLORREF background = RGB(10, 20, 30);
static DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
static int setWallpaperCalls = 0;
static int wallpaperAsks = 0, rectAsks = 0;   // the wallpaper API made (a call into Explorer) and monitor rectangles asked of it
static LPWSTR Copy(const std::wstring& s) {
    auto p = (LPWSTR)CoTaskMemAlloc((s.size() + 1) * sizeof(wchar_t)); wcscpy_s(p, s.size() + 1, s.c_str()); return p;
}
struct FakeWallpaper : IDesktopWallpaper {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDesktopWallpaper)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetWallpaper(LPCWSTR id, LPCWSTR path) override {
        ++setWallpaperCalls;
        if (!id) { for (auto& m : monitors) m.wallpaper = path; return S_OK; }
        Monitor* m = ById(id);
        if (!m || !m->attached) return E_FAIL;
        m->wallpaper = path; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetWallpaper(LPCWSTR id, LPWSTR* path) override {
        Monitor* m = ById(id);
        if (!m) return E_FAIL;
        *path = Copy(m->wallpaper); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMonitorDevicePathAt(UINT i, LPWSTR* id) override {
        if (i >= monitors.size()) return E_INVALIDARG;
        *id = Copy(monitors[i].id); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMonitorDevicePathCount(UINT* n) override { *n = (UINT)monitors.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMonitorRECT(LPCWSTR id, RECT* r) override {
        ++rectAsks;
        Monitor* m = ById(id);
        if (!m || !m->attached) { *r = {}; return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED); }   // known, not attached
        *r = m->rect; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(COLORREF c) override { background = c; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(COLORREF* c) override { *c = background; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPosition(DESKTOP_WALLPAPER_POSITION p) override { position = p; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPosition(DESKTOP_WALLPAPER_POSITION* p) override { *p = position; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetSlideshow(IShellItemArray*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSlideshow(IShellItemArray** items) override { *items = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetSlideshowOptions(DESKTOP_SLIDESHOW_OPTIONS, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSlideshowOptions(DESKTOP_SLIDESHOW_OPTIONS*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AdvanceSlideshow(LPCWSTR, DESKTOP_SLIDESHOW_DIRECTION) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStatus(DESKTOP_SLIDESHOW_STATE* state) override { *state = (DESKTOP_SLIDESHOW_STATE)0; return S_OK; }   // no slideshow
    HRESULT STDMETHODCALLTYPE Enable(BOOL) override { return E_NOTIMPL; }
};
static FakeWallpaper wallpaper;
// The wallpaper API, or nothing (Explorer not running); never the desktop's folder view: the icons stay as they are.
static HRESULT WINAPI FakeCoCreate(REFCLSID clsid, LPUNKNOWN, DWORD, REFIID riid, LPVOID* out) {
    *out = nullptr;
    if (clsid == __uuidof(DesktopWallpaper)) ++wallpaperAsks;
    if (clsid == __uuidof(DesktopWallpaper) && wallpaperApi) return wallpaper.QueryInterface(riid, out);
    return REGDB_E_CLASSNOTREG;
}

// ---- top-level windows and their z-order (top first). The shell's desktop windows stay under HWND_BOTTOM.
struct Win { std::wstring cls; bool visible = true, iconic = false, cloaked = false, desktop = false, defView = false; RECT rect{}; DWORD pid = 500; };
static std::map<HWND, Win> wins;
static std::vector<HWND> z;
static INT_PTR nextHandle = 0x10000;
static HWND shell = nullptr;
static int hooks = 0, unhooks = 0, coverMoves = 0;
static std::vector<std::wstring> events;   // covers made and destroyed, in order
static int Index(HWND h) { auto it = std::find(z.begin(), z.end(), h); return it == z.end() ? -1 : (int)(it - z.begin()); }
static HWND Make(const wchar_t* cls, RECT r, bool desktop = false, DWORD pid = 500) {
    HWND h = (HWND)(nextHandle += 0x10); Win w; w.cls = cls; w.rect = r; w.desktop = desktop; w.pid = pid;
    wins[h] = w; z.push_back(h); return h;
}
static DWORD registerFails = 0, createFails = 0;   // a Windows error: the cover class or windows cannot be made
static HWND WINAPI FakeCreate(DWORD, LPCWSTR cls, LPCWSTR, DWORD style, int x, int y, int cx, int cy, HWND, HMENU, HINSTANCE, LPVOID) {
    if (createFails) { SetLastError(createFails); return nullptr; }
    HWND h = (HWND)(nextHandle += 0x10); Win w; w.cls = cls; w.visible = (style & WS_VISIBLE) != 0; w.rect = {x, y, x + cx, y + cy};
    w.pid = GetCurrentProcessId(); wins[h] = w; z.insert(z.begin(), h);   // a new window starts at the top of its band
    events.push_back(L"make " + std::to_wstring((INT_PTR)h)); return h;
}
static BOOL WINAPI FakeDestroy(HWND h) {
    if (!wins.erase(h)) return FALSE;
    z.erase(std::find(z.begin(), z.end(), h)); events.push_back(L"destroy " + std::to_wstring((INT_PTR)h)); return TRUE;
}
static BOOL WINAPI FakeIsWindow(HWND h) { return wins.count(h) != 0; }
static BOOL WINAPI FakeVisible(HWND h) { auto it = wins.find(h); return it != wins.end() && it->second.visible; }
static BOOL WINAPI FakeIconic(HWND h) { auto it = wins.find(h); return it != wins.end() && it->second.iconic; }
static BOOL WINAPI FakeRect(HWND h, LPRECT r) { auto it = wins.find(h); if (it == wins.end()) return FALSE; *r = it->second.rect; return TRUE; }
static int WINAPI FakeClass(HWND h, LPWSTR out, int n) {
    auto it = wins.find(h); if (it == wins.end() || n <= 0) return 0;
    std::wstring c = it->second.cls.substr(0, n - 1); wcscpy_s(out, n, c.c_str()); return (int)c.size();
}
static DWORD WINAPI FakePid(HWND h, LPDWORD pid) { auto it = wins.find(h); if (pid) *pid = it == wins.end() ? 0 : it->second.pid; return 1; }
static HRESULT WINAPI FakeAttribute(HWND h, DWORD what, PVOID value, DWORD) {
    auto it = wins.find(h); if (it == wins.end()) return E_HANDLE;
    if (what == DWMWA_CLOAKED) { *(DWORD*)value = it->second.cloaked ? 1 : 0; return S_OK; }
    return E_INVALIDARG;
}
static HRESULT WINAPI FakeSetAttribute(HWND, DWORD, LPCVOID, DWORD) { return S_OK; }
static BOOL WINAPI FakeLayered(HWND, COLORREF, BYTE, DWORD) { return TRUE; }
static HWND WINAPI FakeShell() { return shell; }
static HWND WINAPI FakeGetWindow(HWND h, UINT cmd) {
    int i = Index(h); if (i < 0) return nullptr;
    if (cmd == GW_HWNDNEXT) return i + 1 < (int)z.size() ? z[i + 1] : nullptr;
    if (cmd == GW_HWNDPREV) return i > 0 ? z[i - 1] : nullptr;
    return nullptr;
}
static HWND WINAPI FakeFind(HWND parent, HWND after, LPCWSTR cls, LPCWSTR) {
    if (parent) {   // the only child asked for: the desktop's icon view
        auto it = wins.find(parent);
        return it != wins.end() && it->second.defView && cls && wcscmp(cls, L"SHELLDLL_DefView") == 0 ? (HWND)((INT_PTR)parent + 1) : nullptr;
    }
    if (after && Index(after) < 0) return nullptr;
    for (int j = after ? Index(after) + 1 : 0; j < (int)z.size(); ++j)   // the windows below `after`
        if (wins[z[j]].cls == cls) return z[j];
    return nullptr;
}
static bool IsCoverWindow(HWND h) { auto it = wins.find(h); return it != wins.end() && it->second.cls == L"LiquidDockOledCover"; }
static BOOL WINAPI FakePos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!wins.count(h)) return FALSE;
    Win& w = wins[h];
    if (!(flags & SWP_NOMOVE)) OffsetRect(&w.rect, x - w.rect.left, y - w.rect.top);
    if (!(flags & SWP_NOSIZE)) { w.rect.right = w.rect.left + cx; w.rect.bottom = w.rect.top + cy; }
    if (flags & SWP_SHOWWINDOW) w.visible = true;
    if (flags & SWP_NOZORDER) return TRUE;
    if (after != HWND_BOTTOM && after != HWND_TOP && Index(after) < 0) return FALSE;
    if (after == h) return TRUE;
    z.erase(std::find(z.begin(), z.end(), h));
    size_t at = 0;
    if (after == HWND_BOTTOM) { at = z.size(); while (at > 0 && wins[z[at - 1]].desktop) --at; }
    else if (after != HWND_TOP) at = (size_t)Index(after) + 1;
    z.insert(z.begin() + at, h);
    if (IsCoverWindow(h)) ++coverMoves;
    return TRUE;
}
static HWINEVENTHOOK WINAPI FakeHook(DWORD, DWORD, HMODULE, WINEVENTPROC, DWORD, DWORD, DWORD) { ++hooks; return (HWINEVENTHOOK)0x77; }
static BOOL WINAPI FakeUnhook(HWINEVENTHOOK) { ++unhooks; return TRUE; }
static ATOM WINAPI FakeRegisterClass(const WNDCLASSEXW*) {
    if (registerFails) { SetLastError(registerFails); return 0; }
    return (ATOM)0xC001;
}

#define CoCreateInstance FakeCoCreate
#define EnumDisplayMonitors FakeEnumMonitors
#define GetMonitorInfoW FakeMonitorInfo
#define MonitorFromRect FakeMonitorFromRect
#define GetSystemMetrics FakeMetrics
#define CreateWindowExW FakeCreate
#define DestroyWindow FakeDestroy
#define IsWindow FakeIsWindow
#define IsWindowVisible FakeVisible
#define IsIconic FakeIconic
#define GetWindowRect FakeRect
#define GetClassNameW FakeClass
#define GetWindowThreadProcessId FakePid
#define DwmGetWindowAttribute FakeAttribute
#define DwmSetWindowAttribute FakeSetAttribute
#define SetLayeredWindowAttributes FakeLayered
#define GetShellWindow FakeShell
#define GetWindow FakeGetWindow
#define FindWindowExW FakeFind
#define SetWindowPos FakePos
#define SetWinEventHook FakeHook
#define UnhookWinEvent FakeUnhook
#define RegisterClassExW FakeRegisterClass
#include "../src/oled.cpp"
#include "../src/oled_covers.cpp"

static const HMONITOR kMain = (HMONITOR)0x101, kSide = (HMONITOR)0x102;
static const RECT kMainRect{0, 0, 2560, 1440}, kSideRect{-1440, -560, 0, 2000};   // a desktop's two monitors, one of them portrait
static std::wstring Black() { return AppDataDir() + L"\\oled_black.bmp"; }
static std::wstring Record() { return AppDataDir() + L"\\oled.ini"; }
static bool HasRecord() { return GetFileAttributesW(Record().c_str()) != INVALID_FILE_ATTRIBUTES; }
static void Reset() {
    DeleteFileW(Record().c_str()); DeleteFileW((Record() + L".tmp").c_str()); DeleteFileW(Black().c_str());
    monitors = {{kMain, kMainRect, L"\\\\?\\DISPLAY#QDO#main", true, L"C:\\Wallpapers\\main.jpg"},
                {kSide, kSideRect, L"\\\\?\\DISPLAY#SIDE#side", true, L"C:\\Wallpapers\\side.jpg"}};
    wallpaperApi = true; position = DWPOS_FILL; background = RGB(10, 20, 30);
}
// The dock's main loop for `seconds`, 60 passes a second: oled::Tick each pass, then `each` (another program's doing).
static void Run(double seconds, const std::function<void()>& each = {}) {
    for (double end = g_now + seconds; g_now < end; g_now += 1.0 / 60) { oled::Tick(g_now); if (each) each(); }
}
static std::vector<HWND> Covers() { std::vector<HWND> c; for (HWND h : z) if (IsCoverWindow(h)) c.push_back(h); return c; }
static int CountLogs(const wchar_t* part) { int n = 0; for (auto& l : logs) n += l.find(part) != std::wstring::npos; return n; }
// The covers lie together, right above `host`, with every window in `above` over them.
static bool RestRightAbove(HWND host, std::initializer_list<HWND> above) {
    int at = Index(host); auto covers = Covers();
    if (covers.empty() || at < (int)covers.size()) return false;
    for (int k = 1; k <= (int)covers.size(); ++k) if (!IsCoverWindow(z[at - k])) return false;
    for (HWND w : above) if (Index(w) > at - (int)covers.size()) return false;
    return true;
}

int main() {
    // ---- turning it on and off: the record is written first and is what puts the wallpapers back
    {
        Reset();
        Check(oled::Enable(nullptr), "the mode comes on for every monitor");
        Check(HasRecord() && monitors[0].wallpaper == Black() && monitors[1].wallpaper == Black(), "...the record kept, both wallpapers black");
        Check(oled::Active() && background == RGB(0, 0, 0), "...it is on, the background colour black");
        Check(oled::Enable(nullptr) && setWallpaperCalls == 2, "on again: nothing more is changed");
        monitors[1].wallpaper = L"C:\\Wallpapers\\picked meanwhile.jpg";   // the user chose a new one in the meantime
        Check(oled::Disable() && !HasRecord() && !oled::Active(), "off: the record goes with it");
        Check(monitors[0].wallpaper == L"C:\\Wallpapers\\main.jpg" && background == RGB(10, 20, 30), "...the first monitor's wallpaper and the colour come back");
        Check(monitors[1].wallpaper == L"C:\\Wallpapers\\picked meanwhile.jpg", "...a wallpaper picked meanwhile is kept");
    }
    {   // the record survives a restart: a new process reads it back
        Reset();
        oled::Enable(kMain);
        std::ifstream f(Record()); std::string all((std::istreambuf_iterator<char>(f)), {});
        Check(all.find("active=1") != std::string::npos && all.find("allMonitors=0") != std::string::npos &&
              all.find("main.jpg") != std::string::npos && all.find("side.jpg") == std::string::npos, "one monitor: only its wallpaper is recorded");
        f.close();
        oled::Disable();
        Check(monitors[0].wallpaper == L"C:\\Wallpapers\\main.jpg" && monitors[1].wallpaper == L"C:\\Wallpapers\\side.jpg", "...and only it was changed and put back");
    }
    {   // Explorer is not there when the mode is turned off (the restore shortcut is the way out when the shell is broken)
        Reset();
        oled::Enable(nullptr);
        wallpaperApi = false; logs.clear();
        const bool off = oled::Disable();
        Check(HasRecord(), "off while the wallpaper API is unavailable: the record that can still undo it is kept");
        Check(!off && oled::Active() && monitors[0].wallpaper == Black() && CountLogs(L"oled: off failed") == 1,
              "...it says it could not (the dock's button asks to try again), stays on, nothing half undone, dock.log says why");
        wallpaperApi = true;
        g_now += 3;
        const bool offNow = oled::Disable();
        Check(monitors[0].wallpaper == L"C:\\Wallpapers\\main.jpg" && monitors[1].wallpaper == L"C:\\Wallpapers\\side.jpg",
              "...so that turning it off once Explorer is back puts the wallpapers back");
        Check(offNow && !HasRecord() && !oled::Active() && background == RGB(10, 20, 30), "...and then it is off, the colour back, the record gone");
        oled::Disable(); DeleteFileW(Record().c_str()); g_now += 3; oled::Tick(g_now);
    }
    {   // the covers' window class cannot be registered (no USER objects or desktop heap left), before any cover was made
        Reset(); logs.clear();
        registerFails = ERROR_NOT_ENOUGH_MEMORY;
        oled::Enable(nullptr); Run(60);
        Check(Covers().empty() && CountLogs(L"oled: cover class failed 8") == 1,
              "no cover class for a minute: nothing covered, and dock.log says so once, not every 2 s");
        registerFails = ERROR_NOT_ENOUGH_QUOTA; Run(2.1);
        Check(CountLogs(L"oled: cover class failed 1816") == 1, "...a new reason is said");
        oled::InvalidateCovers(); Run(0.1);
        Check(CountLogs(L"oled: cover class failed 1816") == 2, "...and so is the same one after a display change");
        registerFails = 0; oled::Disable(); Run(0.1);
    }

    // ---- the covers: one per monitor in the mode, right above the desktop, under every window
    Reset();
    z.clear(); wins.clear();
    const HWND app = Make(L"Chrome_WidgetWin_1", {100, 100, 1800, 1200});
    const HWND side = Make(L"Notepad", {-1300, 0, -200, 900});
    shell = Make(L"Progman", {-1440, -560, 2560, 2000}, true);   // 24H2+: Progman holds the icons, the live wallpaper inside it
    wins[shell].defView = true;
    {
        oled::Enable(kMain);
        Check(oled::CoversMonitor(kMain) && oled::CoversMonitor(kSide), "just turned on: every dock stays hidden until the covers are worked out");
        Run(0.1);
        Check(Covers().size() == 1 && EqualRect(&wins[Covers()[0]].rect, &kMainRect), "one monitor in the mode: one cover, over it");
        Check(oled::CoversMonitor(kMain) && !oled::CoversMonitor(kSide), "...its dock hidden, the other one's free");
        Check(RestRightAbove(shell, {app, side}) && wins[Covers()[0]].visible, "...shown right above the desktop, under every window");
        Check(hooks == 1, "...with the foreground watched");
        oled::Disable(); Run(0.1);
        Check(Covers().empty() && unhooks == 1 && !oled::CoversMonitor(kMain), "off: the cover and the hook go");
    }
    {   // a monitor unplugged while the mode covers just it (an OLED monitor switched off at its button leaves the desktop)
        Reset();
        oled::Enable(kMain); Run(0.5);
        Check(Covers().size() == 1, "fixture: the main monitor covered");
        logs.clear();
        monitors[0].attached = false; oled::InvalidateCovers();   // as BuildDocks does after the display change
        Run(60);
        Check(Covers().empty() && !oled::CoversMonitor(kSide), "unplugged: no cover, the other monitor's dock shows");
        const int lines = CountLogs(L"oled: 0 cover(s)");
        std::printf("note: %d \"oled: 0 cover(s)\" lines in a minute with nothing to cover (dock.log stops at 20000 lines)\n", lines);
        Check(lines <= 1, "...and dock.log says so once, not every 2 s (the log stops for good after 20000 lines)");
        Check(lines == 1, "...but it does say so");
        logs.clear(); monitors[0].attached = true; oled::InvalidateCovers(); Run(0.1);
        Check(Covers().size() == 1 && oled::CoversMonitor(kMain), "plugged in again: covered again");
        Check(CountLogs(L"oled: 1 cover(s)") == 1, "...and dock.log says so");
        // Explorer gone for a while: in this mode the wallpaper API says which monitor is covered
        logs.clear(); wallpaperApi = false; oled::InvalidateCovers(); Run(60);
        std::printf("note: Explorer gone for a minute: %d \"wallpaper api unavailable\" and %d \"0 cover(s)\" lines\n",
                    CountLogs(L"wallpaper api unavailable"), CountLogs(L"oled: 0 cover(s)"));
        Check(CountLogs(L"wallpaper api unavailable") <= 1 && CountLogs(L"oled: 0 cover(s)") <= 1, "Explorer gone for a minute: dock.log says so once, not every 2 s");
        Check(CountLogs(L"wallpaper api unavailable") == 1 && CountLogs(L"oled: 0 cover(s)") == 1, "...but it does say so");
        logs.clear();
        Check(!oled::Disable() && CountLogs(L"wallpaper api unavailable") == 1 && CountLogs(L"oled: off failed") == 1,
              "...turning the mode off meanwhile fails, and dock.log says why, whatever the retries said before");
        logs.clear(); wallpaperApi = true; Run(2.1);   // Explorer back (TaskbarCreated): no display change, the retry finds it
        Check(Covers().size() == 1 && CountLogs(L"oled: 1 cover(s)") == 1, "Explorer back: within 2 s the monitor is covered again, and dock.log says so");
        wallpaperApi = true; oled::InvalidateCovers(); Run(0.1);
        Check(Covers().size() == 1, "...and the monitor is covered again once it is back");
        oled::Disable(); Run(0.1);
    }
    {   // the monitor the mode covers switched off for an hour: it comes back with a display change, which rebuilds the
        // covers at once - after 30 s Explorer is asked about it once a minute, not every 2 s
        Reset();
        oled::Enable(kMain); Run(0.5);
        Check(Covers().size() == 1, "fixture: the main monitor covered");
        logs.clear();
        monitors[0].attached = false; oled::InvalidateCovers();
        const int asks = wallpaperAsks, rects = rectAsks;
        Run(3600);
        std::printf("note: an hour with the covered monitor switched off: %d wallpaper API calls, %d monitor rectangles asked of Explorer\n",
                    wallpaperAsks - asks, rectAsks - rects);
        Check(Covers().empty() && !oled::CoversMonitor(kSide) && wallpaperAsks - asks <= 80,
              "an hour with the mode's monitor switched off: no cover, the other dock free, Explorer asked some 75 times, not 1800");
        Check(wallpaperAsks - asks >= 70 && CountLogs(L"oled: 0 cover(s)") == 1, "...still looked for once a minute; dock.log says it once");
        logs.clear(); monitors[0].attached = true; oled::InvalidateCovers(); Run(0.1);
        Check(Covers().size() == 1 && CountLogs(L"oled: 1 cover(s)") == 1, "...switched on again (a display change): covered at once, and dock.log says so");
        // back without a display change (none came): the minute's look finds it
        monitors[0].attached = false; oled::InvalidateCovers(); Run(120);
        monitors[0].attached = true;
        double from = g_now;
        while (Covers().empty() && g_now < from + 120) Run(1.0 / 60);
        Check(Covers().size() == 1 && g_now - from <= 60.05, "...back with no display change: covered within a minute");
        // in the first 30 s after a change every 2 s, as before (Explorer may know a new monitor a moment after Windows)
        monitors[0].attached = false; oled::InvalidateCovers(); Run(25);
        monitors[0].attached = true; from = g_now;
        while (Covers().empty() && g_now < from + 10) Run(1.0 / 60);
        Check(Covers().size() == 1 && g_now - from <= 2.05, "...back within 30 s of the change: covered within 2 s, as before");
        // Explorer away for five minutes: no event when it is back (its TaskbarCreated does not rebuild the covers), so every 2 s
        wallpaperApi = false; oled::InvalidateCovers(); Run(300);
        wallpaperApi = true; from = g_now;
        while (Covers().empty() && g_now < from + 10) Run(1.0 / 60);
        Check(Covers().size() == 1 && g_now - from <= 2.05, "Explorer away for five minutes: covered within 2 s of its return, as before");
        oled::Disable(); Run(0.1);
    }
    {   // the cover windows cannot be made (no USER objects or desktop heap left): nothing covered, the 2 s retry goes on
        Reset(); logs.clear();
        createFails = ERROR_NOT_ENOUGH_MEMORY;
        oled::Enable(nullptr); Run(60);
        std::printf("note: %d \"cover window failed\" lines in a minute with no cover window to be made (2 monitors)\n",
                    CountLogs(L"cover window failed"));
        Check(Covers().empty() && CountLogs(L"oled: cover window failed 8") == 2 && CountLogs(L"oled: 0 cover(s)") == 1,
              "no cover window for a minute: dock.log says so once for each monitor, not every 2 s");
        createFails = ERROR_NOT_ENOUGH_QUOTA; Run(2.1);
        Check(CountLogs(L"oled: cover window failed 1816") == 2, "...a new reason is said");
        logs.clear(); oled::InvalidateCovers(); Run(0.1);
        Check(CountLogs(L"oled: cover window failed 1816") == 2, "...and so is the same one after a display change");
        logs.clear(); createFails = 0; Run(2.1);
        Check(Covers().size() == 2 && CountLogs(L"oled: 2 cover(s)") == 1 && CountLogs(L"cover window failed") == 0,
              "...once they can be made again: covered within 2 s, and dock.log says so");
        oled::Disable(); Run(0.1);
    }
    {   // monitors changed: the new covers are up before the old ones go
        Reset();
        oled::Enable(nullptr); Run(0.1);
        auto before = Covers();
        Check(before.size() == 2, "every monitor: two covers");
        events.clear(); logs.clear(); oled::InvalidateCovers(); Run(0.1);
        Check(CountLogs(L"oled: 2 cover(s)") == 1, "rebuilt with as many covers as before: dock.log has its line, as after every change");
        Check(events.size() == 4 && events[0].rfind(L"make", 0) == 0 && events[1].rfind(L"make", 0) == 0 &&
              events[2].rfind(L"destroy", 0) == 0 && events[3].rfind(L"destroy", 0) == 0, "rebuilt: the new covers are made before the old ones go");
        Check(Covers().size() == 2 && RestRightAbove(shell, {app, side}), "...and rest above the desktop like the old ones");
        // the record was taken away by another process (--restore): within the 2 s cache the covers go
        DeleteFileW(Record().c_str());
        Run(1.0); Check(Covers().size() == 2, "the record gone: the cache still says on for a moment");
        Run(1.5); Check(Covers().empty(), "...and within 2 s the covers are gone");
    }
    {   // "show desktop" lifts the desktop over the windows: the covers follow, and come down again after
        Reset();
        oled::Enable(nullptr); Run(0.2);
        // the desktop is lifted over the windows (and over the covers lying at the bottom)
        z.erase(std::find(z.begin(), z.end(), shell)); z.insert(z.begin(), shell);
        oled::OnForeground(nullptr, EVENT_SYSTEM_FOREGROUND, shell, 0, 0, 0, 0);
        Check(Index(Covers()[0]) == 0 && Index(Covers()[1]) == 1 && Index(shell) == 2 && oled::g_raised, "show desktop: the covers right above the lifted desktop");
        Run(1.0);
        Check(Index(shell) == 2 && Index(app) > 2 && oled::g_raised, "...and stay there while it lasts");
        // it ends: the desktop goes back to the bottom; the windows are over it, the covers left on top of them
        z.erase(std::find(z.begin(), z.end(), shell)); z.push_back(shell);
        oled::OnForeground(nullptr, EVENT_SYSTEM_FOREGROUND, app, 0, 0, 0, 0);
        Check(RestRightAbove(shell, {app, side}) && !oled::g_raised, "show desktop over: the covers back under the windows");
        // windows under the covers that the black would hide: minimized, cloaked or hidden ones do not count
        const HWND hidden = Make(L"Hidden", {0, 0, 500, 500}), cloaked = Make(L"Cloaked", {0, 0, 500, 500}), minimized = Make(L"Mini", {-32000, -32000, -31840, -31972});
        wins[hidden].visible = false; wins[cloaked].cloaked = true; wins[minimized].iconic = true;
        for (HWND h : {hidden, cloaked, minimized}) { z.erase(std::find(z.begin(), z.end(), h)); z.insert(z.end() - 1, h); }
        const int moves = coverMoves; Run(1.0);
        Check(coverMoves == moves, "hidden, cloaked and minimized windows under the covers leave them where they are");
        for (HWND h : {hidden, cloaked, minimized}) DestroyWindow(h);
        oled::Disable(); Run(0.1);
    }
    {   // another program keeps putting its window at the very bottom: 20 times back, then 30 s of rest
        Reset();
        oled::Enable(nullptr); Run(0.2);
        const HWND widget = Make(L"RainmeterMeterWindow", {2200, 100, 2500, 400});
        auto sink = [&] { if (Index(widget) + 1 != Index(shell)) { z.erase(std::find(z.begin(), z.end(), widget)); z.insert(z.end() - 1, widget); } };
        sink(); logs.clear();
        const int start = coverMoves;
        Run(3.0, sink);
        Check((coverMoves - start) / 2 == 20 && CountLogs(L"covers paused for 30 s") == 1, "a window that keeps taking the bottom: put back 20 times, then the covers pause");
        const int paused = coverMoves;
        Run(28.0, sink);
        Check(coverMoves == paused, "...for 30 s nothing is moved");
        Run(3.0, sink);
        Check(coverMoves > paused, "...then they look again");
        DestroyWindow(widget);
        oled::Disable(); Run(0.1);
    }
    {   // before 24H2, with a live wallpaper: the icons in a WorkerW of their own over the wallpaper's WorkerW
        Reset();
        z.erase(std::find(z.begin(), z.end(), shell)); wins[shell].defView = false; z.push_back(shell);
        const DWORD explorer = wins[shell].pid;
        const HWND icons = Make(L"WorkerW", {-1440, -560, 2560, 2000}, true, explorer), wall = Make(L"WorkerW", {-1440, -560, 2560, 2000}, true, explorer);
        wins[icons].defView = true;
        for (HWND h : {icons, wall}) z.erase(std::find(z.begin(), z.end(), h));
        z.insert(z.end() - 1, icons); z.insert(z.end() - 1, wall);   // ... icons, wallpaper, Progman
        oled::Enable(nullptr); Run(0.2);
        Check(RestRightAbove(icons, {app, side}), "before 24H2: the covers right above the icons' WorkerW, under every window");
        const int moves = coverMoves; Run(1.0);
        Check(coverMoves == moves, "...and left there");
        oled::Disable(); Run(0.1);
        for (HWND h : {icons, wall}) DestroyWindow(h);
    }
    {   // Explorer restarting: nothing to put the covers above, nothing is moved
        Reset();
        oled::Enable(nullptr); Run(0.2);
        const HWND was = shell; shell = nullptr;
        const int moves = coverMoves; Run(1.0);
        Check(coverMoves == moves && Covers().size() == 2, "no desktop (Explorer restarting): the covers stay and nothing is moved");
        shell = was;
        oled::Disable(); Run(0.1);
    }
    DeleteFileW(Record().c_str()); DeleteFileW(Black().c_str()); RemoveDirectoryW(AppDataDir().c_str());
    return Summary("preview_oled_test");
}
