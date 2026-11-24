#include "oled.h"
#include "oled_internal.h"   // also <shobjidl.h> (IDesktopWallpaper); ComPtr: common.h
#include <shlobj.h>
#include <exdisp.h>
#include <shlguid.h>
#include <fstream>

namespace oled {
namespace {

std::wstring StatePath() { return AppDataDir() + L"\\oled.ini"; }
std::wstring BlackPath() { return AppDataDir() + L"\\oled_black.bmp"; }

}   // namespace

State Load() {
    State s;
    std::ifstream f(StatePath(), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::wstring v = FromUtf8(line.substr(eq + 1));
        try {
            if (k == "active") s.active = std::stoi(v) != 0;
            else if (k == "iconsHidden") s.iconsHidden = std::stoi(v) != 0;
            else if (k == "allMonitors") s.allMonitors = std::stoi(v) != 0;
            else if (k == "background") s.background = (COLORREF)std::stoul(v);
            else if (k == "slideshow") s.slideshow = v;
            else if (k == "wallpaper") {
                auto tab = v.find(L'\t');
                if (tab != std::wstring::npos) s.wallpapers[v.substr(0, tab)] = v.substr(tab + 1);
            }
        } catch (...) {}
    }
    return s;
}

namespace {

// The way back to the user's wallpapers: written whole or not at all (common.h SaveFileWhole).
bool Save(const State& s) {
    std::string text = "active=" + std::to_string(s.active ? 1 : 0) + "\n" +
                       "iconsHidden=" + std::to_string(s.iconsHidden ? 1 : 0) + "\n" +
                       "allMonitors=" + std::to_string(s.allMonitors ? 1 : 0) + "\n" +
                       "background=" + std::to_string((unsigned long)s.background) + "\n" +
                       "slideshow=" + ToUtf8(s.slideshow) + "\n";
    for (auto& [mon, path] : s.wallpapers) text += "wallpaper=" + ToUtf8(mon + L"\t" + path) + "\n";
    return SaveFileWhole(StatePath(), text, L"oled");
}

// A tiny black bitmap; the wallpaper fit modes stretch or centre it on a black background colour.
bool EnsureBlackBitmap() {
    if (GetFileAttributesW(BlackPath().c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    constexpr int kSide = 16;
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = kSide; ih.biHeight = kSide; ih.biPlanes = 1; ih.biBitCount = 24;
    ih.biSizeImage = kSide * kSide * 3;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    std::ofstream f(BlackPath(), std::ios::binary | std::ios::trunc);
    f.write((const char*)&fh, sizeof(fh));
    f.write((const char*)&ih, sizeof(ih));
    std::string pixels(ih.biSizeImage, '\0');
    f.write(pixels.data(), pixels.size());
    return f.good();
}

HRESULT g_wallpaperLast = S_OK;   // how the last ask for the wallpaper API went (S_OK: it was there)

}   // namespace

// Explorer's wallpaper API; none while Explorer is not running. retry: the covers' 2 s retry, which asks
// again and again while Explorer is away - a line when that starts, not on every try.
ComPtr<IDesktopWallpaper> Wallpaper(bool retry) {
    ComPtr<IDesktopWallpaper> w;
    HRESULT hr = CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&w));
    if (FAILED(hr) && (!retry || hr != g_wallpaperLast)) Log(L"oled: wallpaper api unavailable %08x", hr);
    g_wallpaperLast = FAILED(hr) ? hr : S_OK;
    return w;
}

namespace {

std::vector<std::wstring> MonitorIds(IDesktopWallpaper* w) {
    std::vector<std::wstring> ids;
    UINT n = 0;
    if (FAILED(w->GetMonitorDevicePathCount(&n))) return ids;
    for (UINT i = 0; i < n; ++i) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(w->GetMonitorDevicePathAt(i, &id)) && id) {
            RECT r{};
            if (SUCCEEDED(w->GetMonitorRECT(id, &r))) ids.push_back(id);   // skip detached monitors
            CoTaskMemFree(id);
        }
    }
    return ids;
}

std::wstring MonitorIdFor(IDesktopWallpaper* w, HMONITOR monitor) {
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(monitor, &mi)) return {};
    for (auto& id : MonitorIds(w)) {
        RECT r{};
        if (SUCCEEDED(w->GetMonitorRECT(id.c_str(), &r)) && EqualRect(&r, &mi.rcMonitor)) return id;
    }
    return {};
}

std::wstring CurrentWallpaper(IDesktopWallpaper* w, const std::wstring& id) {
    LPWSTR path = nullptr;
    std::wstring s;
    if (SUCCEEDED(w->GetWallpaper(id.c_str(), &path)) && path) { s = path; CoTaskMemFree(path); }
    return s;
}

std::wstring SlideshowFolder(IDesktopWallpaper* w) {
    DESKTOP_SLIDESHOW_STATE st{};
    if (FAILED(w->GetStatus(&st)) || !(st & DSS_ENABLED)) return {};
    ComPtr<IShellItemArray> items;
    if (FAILED(w->GetSlideshow(&items)) || !items) return {};
    ComPtr<IShellItem> item;
    if (FAILED(items->GetItemAt(0, &item))) return {};
    LPWSTR path = nullptr;
    std::wstring s;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) { s = path; CoTaskMemFree(path); }
    return s;
}

// The desktop's folder view (the documented way to toggle "show desktop icons").
ComPtr<IFolderView2> DesktopView() {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return nullptr;
    VARIANT loc{}; loc.vt = VT_I4; loc.lVal = CSIDL_DESKTOP;
    VARIANT empty{}; long hwnd = 0;
    ComPtr<IDispatch> disp;
    if (windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK || !disp) return nullptr;
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> fv;
    if (FAILED(disp.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&fv))) return nullptr;
    return fv;
}

// returns the previous "icons visible" state, or -1 on failure
int SetIconsVisible(bool visible) {
    auto fv = DesktopView();
    DWORD flags = 0;
    if (!fv || FAILED(fv->GetCurrentFolderFlags(&flags))) { Log(L"oled: desktop view unavailable"); return -1; }
    int was = (flags & FWF_NOICONS) ? 0 : 1;
    if (was != (visible ? 1 : 0) && FAILED(fv->SetCurrentFolderFlags(FWF_NOICONS, visible ? 0 : FWF_NOICONS))) return -1;
    return was;
}

}   // namespace

// cached: the button reads it every frame
static double g_checkedAt = -1;
static bool g_active = false;
static void SetCached(bool on) { g_active = on; g_checkedAt = NowSeconds(); }

bool Active() {
    double now = NowSeconds();
    if (g_checkedAt < 0 || now - g_checkedAt > 2.0) SetCached(Load().active);
    return g_active;
}

bool Enable(HMONITOR monitor) {
    State s = Load();
    if (s.active) return true;
    auto w = Wallpaper();
    if (!w || !EnsureBlackBitmap()) return false;

    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    w->GetPosition(&pos);
    std::vector<std::wstring> targets;
    if (monitor && pos != DWPOS_SPAN) {
        std::wstring id = MonitorIdFor(w.Get(), monitor);
        if (!id.empty()) targets.push_back(id);
    }
    s.allMonitors = targets.empty();
    if (targets.empty()) targets = MonitorIds(w.Get());
    if (targets.empty()) return false;

    s.slideshow = SlideshowFolder(w.Get());
    w->GetBackgroundColor(&s.background);
    for (auto& id : targets) s.wallpapers[id] = CurrentWallpaper(w.Get(), id);
    s.active = true;
    // write the recovery record before touching anything
    if (!Save(s)) { Log(L"oled: could not save state"); return false; }

    w->SetBackgroundColor(RGB(0, 0, 0));
    bool ok = true;
    if (pos == DWPOS_SPAN) ok = SUCCEEDED(w->SetWallpaper(nullptr, BlackPath().c_str()));
    else for (auto& id : targets) if (FAILED(w->SetWallpaper(id.c_str(), BlackPath().c_str()))) ok = false;

    int was = SetIconsVisible(false);
    s.iconsHidden = was == 1;
    Save(s);
    Log(L"oled: on (%s, %d monitor(s), wallpaper %d, icons %d)", s.allMonitors ? L"all" : L"one", (int)targets.size(), (int)ok, was);
    SetCached(true);
    InvalidateCovers();   // the next Tick puts the covers up
    return true;
}

bool Disable() {
    State s = Load();
    if (!s.active) return true;
    // Explorer is not there (not running, or restarting): nothing can be put back now. The record is the only
    // way back to the user's wallpapers, so it stays, with the mode on, for the next try (the button, --restore).
    auto w = Wallpaper();
    if (!w) { Log(L"oled: off failed (no wallpaper api) - kept on for the next try"); return false; }
    if (s.iconsHidden) SetIconsVisible(true);
    w->SetBackgroundColor(s.background);
    std::wstring black = ToLower(BlackPath());
    for (auto& [id, original] : s.wallpapers) {
        // the user may have picked a new wallpaper meanwhile: keep that one
        if (ToLower(CurrentWallpaper(w.Get(), id)) != black) continue;
        w->SetWallpaper(id.c_str(), original.c_str());
    }
    if (!s.slideshow.empty()) {
        ComPtr<IShellItem> folder;
        ComPtr<IShellItemArray> items;
        if (SUCCEEDED(SHCreateItemFromParsingName(s.slideshow.c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
            SUCCEEDED(SHCreateShellItemArrayFromShellItem(folder.Get(), IID_PPV_ARGS(&items))))
            w->SetSlideshow(items.Get());
    }
    DeleteFileW(StatePath().c_str());
    SetCached(false);
    DestroyCovers();   // and built again once the mode is back on
    Log(L"oled: off");
    return true;
}

bool SetDesktopIconsVisible(bool visible) { return SetIconsVisible(visible) >= 0; }

void Reapply() {
    State s = Load();
    if (s.active && s.iconsHidden) SetIconsVisible(false);
}

}   // namespace oled
