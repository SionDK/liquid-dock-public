#include "apps.h"
#include "apps_internal.h"
#include "gfx.h"
#include "icon_pixels.h"   // AnyAlpha, MakeOpaque
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwmapi.h>
#include <appmodel.h>

AppModel g_apps;

// ---------------------------------------------------------------- helpers

std::wstring ProcessExe(DWORD pid) {
    std::wstring out;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) {
        wchar_t buf[MAX_PATH * 2]; DWORD n = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
        CloseHandle(p);
    }
    return ToLower(out);
}

static std::wstring ProcessAumid(DWORD pid) {
    std::wstring out;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) {
        wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH]; UINT32 n = ARRAYSIZE(buf);
        if (GetApplicationUserModelId(p, &n, buf) == ERROR_SUCCESS) out = buf;
        CloseHandle(p);
    }
    return out;
}

// The AppUserModelID a property store holds (a window's, a shortcut's) put in out; out is left as it was when the
// store holds none.
void ReadAumid(IPropertyStore* ps, std::wstring& out) {
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) out = pv.pwszVal;
    PropVariantClear(&pv);
}

static std::wstring WindowAumid(HWND h) {
    std::wstring out;
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) ReadAumid(ps.Get(), out);
    return out;
}

bool IsCloaked(HWND h) { return CloakOf(h) != 0; }

DWORD CloakOf(HWND h) {
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked;
}

// The same heuristic the shell uses to decide whether a window gets a taskbar button.
bool IsTaskbarWindow(HWND h) {
    if (!IsWindowVisible(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    HWND owner = GetWindow(h, GW_OWNER);
    if (ex & WS_EX_APPWINDOW) { /* always */ }
    else {
        if (owner) return false;
        if (ex & WS_EX_TOOLWINDOW) return false;
        if (ex & WS_EX_NOACTIVATE) return false;
    }
    if (IsCloaked(h)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[256] = {};   // a buffer, not a string: read every refresh for each window that gets this far (the listed ones, a few shell windows)
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return false;
    if (GetWindowTextLengthW(h) == 0 && wcscmp(cls, L"CabinetWClass")) return false;
    return true;
}

static std::wstring FileDescription(const std::wstring& path) {
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return {};
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return {};
    struct LangCp { WORD lang, cp; } *tr = nullptr; UINT trLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &trLen) || trLen < sizeof(LangCp)) return {};
    wchar_t q[128];
    swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
    wchar_t* val = nullptr; UINT vlen = 0;
    if (VerQueryValueW(buf.data(), q, (void**)&val, &vlen) && vlen > 1) return std::wstring(val);
    return {};
}

std::wstring FileStem(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    std::wstring n = s == std::wstring::npos ? path : path.substr(s + 1);
    size_t d = n.find_last_of(L'.');
    if (d != std::wstring::npos) n = n.substr(0, d);
    return n;
}

bool EndsWithI(const std::wstring& s, const wchar_t* suf) {
    size_t n = wcslen(suf);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suf) == 0;
}

// The name of a program without a file description (or of a file or folder pinned): its file's name as the disk
// spells it - the dock's paths are lowercased for comparing (ProcessExe, PinFile) - else as its pin spells it.
static std::wstring SpelledName(const std::wstring& exe, const std::wstring& launch) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(exe.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        return (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? std::wstring(fd.cFileName) : FileStem(fd.cFileName);   // a folder has no extension
    }
    return FileStem(!launch.empty() && ToLower(launch) == exe ? launch : exe);
}

// Straight alpha to premultiplied, every pixel: iconpx::Premultiply's arithmetic (icon_pixels.h) without its skipping
// of opaque pixels - a loop with no branch, which the compiler vectorizes, for icons up to 256 x 256 (iconpx's took up
// to about three times as long on an icon's mix of clear, opaque and edge pixels, measured on 2026-11-21).
static void PremultiplyAll(std::vector<uint32_t>& px) {
    for (auto& p : px) {
        uint32_t a = p >> 24;
        uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
        p = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

static ComPtr<ID2D1Bitmap1> BitmapFromHBITMAP(HBITMAP hbm) {
    DIBSECTION ds = {};
    if (!GetObjectW(hbm, sizeof(ds), &ds) || !ds.dsBm.bmBits || ds.dsBm.bmBitsPixel != 32) {
        // not a DIB section: copy through GetDIBits
        BITMAP bm = {};
        if (!GetObjectW(hbm, sizeof(bm), &bm)) return nullptr;
        std::vector<uint32_t> px(bm.bmWidth * bm.bmHeight);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth; bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        HDC hdc = GetDC(nullptr);
        GetDIBits(hdc, hbm, 0, bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, hdc);
        if (!iconpx::AnyAlpha(px)) iconpx::MakeOpaque(px);   // (not premultiplied here)
        return BitmapFromPixels(g_gfx.dc, px, bm.bmWidth, bm.bmHeight);
    }
    int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
    bool bottomUp = ds.dsBmih.biHeight > 0;
    std::vector<uint32_t> px(w * h);
    for (int y = 0; y < h; ++y) {
        const uint32_t* src = (const uint32_t*)((const BYTE*)ds.dsBm.bmBits + ds.dsBm.bmWidthBytes * (bottomUp ? (h - 1 - y) : y));
        memcpy(&px[y * w], src, w * 4);
    }
    bool anyA = false, needPremul = false;
    for (auto p : px) {
        uint32_t a = p >> 24;
        if (a) anyA = true;
        if (((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a) needPremul = true;
    }
    if (!anyA) iconpx::MakeOpaque(px);
    else if (needPremul) PremultiplyAll(px);
    return BitmapFromPixels(g_gfx.dc, px, w, h);
}

static ComPtr<ID2D1Bitmap1> IconFromShellItem(IShellItem* si) {
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(si->QueryInterface(IID_PPV_ARGS(&f)))) return nullptr;
    HBITMAP hbm = nullptr;
    if (FAILED(f->GetImage({ 256, 256 }, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbm)) || !hbm) return nullptr;
    auto b = BitmapFromHBITMAP(hbm);
    DeleteObject(hbm);
    return b;
}

// HICON -> premultiplied D2D bitmap at the icon's native size (handles alpha and mask-only icons).
static ComPtr<ID2D1Bitmap1> IconFromHICON(HICON hi) {
    if (!hi) return nullptr;
    ICONINFO ii = {};
    if (!GetIconInfo(hi, &ii)) return nullptr;
    ComPtr<ID2D1Bitmap1> result;
    BITMAP bm = {};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0) {
        int w = bm.bmWidth, h = bm.bmHeight;
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        std::vector<uint32_t> px(w * h), mask(w * h);
        HDC hdc = GetDC(nullptr);
        GetDIBits(hdc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS);
        if (!iconpx::AnyAlpha(px) && ii.hbmMask) {   // no alpha: the mask says (no mask: left as it is)
            GetDIBits(hdc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS);
            for (int i = 0; i < w * h; ++i) px[i] = (px[i] & 0xffffff) | ((mask[i] & 0xffffff) ? 0 : 0xff000000u);
        }
        ReleaseDC(nullptr, hdc);
        PremultiplyAll(px);   // always (its alpha taken as straight, as Windows icons have it)
        result = BitmapFromPixels(g_gfx.dc, px, w, h);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return result;
}

// Largest icon in a PE/ico file (what the taskbar uses); avoids the shell's "small icon in a frame" thumbnails.
static ComPtr<ID2D1Bitmap1> IconFromFile(const std::wstring& path, int index) {
    if (path.empty()) return nullptr;
    wchar_t expanded[MAX_PATH * 2];
    ExpandEnvironmentStringsW(path.c_str(), expanded, ARRAYSIZE(expanded));
    HICON hi = nullptr; UINT id = 0;
    if (PrivateExtractIconsW(expanded, index, 256, 256, &hi, &id, 1, LR_DEFAULTCOLOR) != 1 || !hi) return nullptr;
    auto b = IconFromHICON(hi);
    DestroyIcon(hi);
    return b;
}

ComPtr<IShellItem> AppsFolderItem(const std::wstring& aumid, std::wstring* nameOut) {
    if (aumid.empty()) return nullptr;
    ComPtr<IShellItem> si;
    if (FAILED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, aumid.c_str(), IID_PPV_ARGS(&si))))
        return nullptr;
    PWSTR dn = nullptr;
    std::wstring name;
    if (SUCCEEDED(si->GetDisplayName(SIGDN_NORMALDISPLAY, &dn))) { name = dn; CoTaskMemFree(dn); }
    if (name.empty() || name == aumid) return nullptr;   // not a real Start-menu entry
    if (nameOut) *nameOut = name;
    return si;
}

// What starts an app by its id (a store app's pin, Launch with no launch): its Apps folder item. pins.txt keeps it as
// the pin's launch, so its spelling must not change.
std::wstring AppsFolderTarget(const std::wstring& aumid) { return L"shell:AppsFolder\\" + aumid; }

// ---------------------------------------------------------------- model

std::shared_ptr<AppItem> AppModel::NewItem() {
    auto a = std::make_shared<AppItem>();
    a->id = nextId++;
    return a;
}

// A shortcut (.lnk) read from its file: false when the shell's link object could not be made or the file not read.
// As's answer is not looked at (the shell's link object has IPersistFile).
bool OpenShortcut(const std::wstring& lnk, Shortcut& s) {
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s.link)))) return false;
    s.link.As(&s.file);
    return SUCCEEDED(s.file->Load(lnk.c_str(), STGM_READ));
}

static bool LinkIconLocation(const std::wstring& lnk, std::wstring& path, int& index) {
    Shortcut s;
    if (!OpenShortcut(lnk, s)) return false;
    wchar_t p[MAX_PATH * 2] = {};
    if (FAILED(s.link->GetIconLocation(p, ARRAYSIZE(p), &index)) || !p[0]) return false;
    path = p;
    return true;
}

void AppModel::LoadIconAndName(AppItem& a, HWND sample) {
    std::wstring nm;
    bool packaged = a.exe.empty() || a.exe.find(L"\\windowsapps\\") != std::wstring::npos;
    if (!packaged) {
        std::wstring ip; int ii = 0;
        if (EndsWithI(a.launch, L".lnk") && LinkIconLocation(a.launch, ip, ii)) a.icon = IconFromFile(ip, ii);
        if (!a.icon) a.icon = IconFromFile(a.exe, 0);
    }
    if (!a.icon && !a.aumid.empty()) {
        if (auto si = AppsFolderItem(a.aumid, &nm)) {
            a.icon = IconFromShellItem(si.Get());
            if (a.name.empty()) a.name = nm;
        }
    }
    if (!a.icon && EndsWithI(a.launch, L".lnk")) {
        ComPtr<IShellItem> si;
        if (SUCCEEDED(SHCreateItemFromParsingName(a.launch.c_str(), nullptr, IID_PPV_ARGS(&si)))) a.icon = IconFromShellItem(si.Get());
    }
    if (!a.icon && !a.exe.empty()) {
        ComPtr<IShellItem> si;
        if (SUCCEEDED(SHCreateItemFromParsingName(a.exe.c_str(), nullptr, IID_PPV_ARGS(&si)))) a.icon = IconFromShellItem(si.Get());
    }
    if (!a.icon && sample) {
        HICON hi = nullptr;
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(sample, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &res) && res) hi = (HICON)res;
        if (!hi) hi = (HICON)GetClassLongPtrW(sample, GCLP_HICON);
        a.icon = IconFromHICON(hi);
    }
    if (a.name.empty() && !a.exe.empty()) {
        a.name = FileDescription(a.exe);
        if (a.name.empty()) a.name = SpelledName(a.exe, a.launch);
    }
    if (a.name.empty() && sample) {
        wchar_t t[256] = {}; GetWindowTextW(sample, t, 256); a.name = t;
    }
}

void AppModel::ReloadIcons() {
    for (auto& a : items) {
        a->icon.Reset();
        LoadIconAndName(*a, a->windows.empty() ? nullptr : a->windows.front());
    }
    structureVersion++;
}

bool g_demoMode = false;

void AppModel::Init() {
    LoadPins();
    MigratePins();
    Refresh();
}

int AppModel::PinnedCount() const {
    int n = 0; for (auto& a : items) if (a->pinned) ++n; return n;
}

std::shared_ptr<AppItem> AppModel::FindByWindow(HWND h) {
    for (auto& a : items)
        for (HWND w : a->windows) if (w == h) return a;
    return nullptr;
}

// What a window is - its program, its id, its package - read when Refresh first sees it (winCache keeps it after).
// false: not known yet, and not kept: a UWP frame whose app is suspended, looked at again at the next refresh.
bool AppModel::IdentifyWindow(HWND h, WinCache& wc) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    wc.exe = ProcessExe(pid);
    wc.aumid = WindowAumid(h);
    std::wstring procAumid = ProcessAumid(pid);
    if (wc.aumid.empty()) wc.aumid = procAumid;
    // packaged apps may label their windows with their own AUMID; the package identifies them
    wc.family = PackageFamily(!procAumid.empty() ? procAumid : wc.aumid);
    if (EndsWithI(wc.exe, L"\\applicationframehost.exe")) {
        // UWP: the real app lives in a child CoreWindow from another process
        struct Ctx { DWORD host; DWORD found; } c{ pid, 0 };
        EnumChildWindows(h, [](HWND ch, LPARAM lp) -> BOOL {
            auto* c = (Ctx*)lp; DWORD p = 0; GetWindowThreadProcessId(ch, &p);
            if (p != c->host) { c->found = p; return FALSE; }
            return TRUE;
        }, (LPARAM)&c);
        if (c.found) {
            wc.exe = ProcessExe(c.found);
            std::wstring pa = ProcessAumid(c.found);
            if (wc.aumid.empty()) wc.aumid = pa;
            if (!pa.empty()) wc.family = PackageFamily(pa);
        }
        if (wc.aumid.empty()) return false;   // suspended frame we can't identify yet
    }
    return true;
}

// The same app, by its AppUserModelID: the ids alike but for case (an id given - no id is no app's).
bool SameAppId(const std::wstring& itemId, const std::wstring& id) {
    return !id.empty() && _wcsicmp(itemId.c_str(), id.c_str()) == 0;
}

// The same app, by its program while one of the two has no id (a program under another id - a browser profile's -
// is an app of its own). The programs are lowercased paths (ProcessExe, PinFile).
bool SameProgramOneIdless(const std::wstring& itemExe, const std::wstring& itemId, const std::wstring& exe, const std::wstring& id) {
    return !exe.empty() && itemExe == exe && (itemId.empty() || id.empty());
}

// The dock's app a window belongs to: the first app with its id, else the first of its package, else the first of
// its program while one of the two has no id - each asked of all the apps before the next (PinFile asks id and
// program of each app at once, and not the package). None: an app new to the dock.
std::shared_ptr<AppItem> AppModel::ItemFor(const WinCache& wc) const {
    std::shared_ptr<AppItem> match;
    if (!wc.aumid.empty())
        for (auto& a : items) if (SameAppId(a->aumid, wc.aumid)) { match = a; break; }
    if (!match && !wc.family.empty())
        for (auto& a : items) if (!a->aumid.empty() && PackageFamily(a->aumid) == wc.family) { match = a; break; }
    if (!match && !wc.exe.empty())
        for (auto& a : items)
            if (SameProgramOneIdless(a->exe, a->aumid, wc.exe, wc.aumid)) { match = a; break; }
    return match;
}

// A launched app bounces until a new window of it shows, or this long at most (s).
static constexpr double kLaunchBounceMax = 8.0;

void AppModel::Refresh() {
    lastRefresh = NowSeconds();
    dirty = false;

    std::vector<HWND> wins;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (IsTaskbarWindow(h)) ((std::vector<HWND>*)lp)->push_back(h);
        return TRUE;
    }, (LPARAM)&wins);

    std::unordered_map<HWND, WinCache> newCache;
    for (auto& a : items) a->windows.clear();

    bool structural = false;
    for (HWND h : wins) {
        auto f = winCache.find(h);
        if (f == winCache.end()) {
            WinCache wc;
            if (!IdentifyWindow(h, wc)) continue;   // not known yet: not kept, asked again next time
            f = winCache.emplace(h, std::move(wc)).first;
        }
        // moved over to this refresh's cache, node and all: a window's facts are not copied every refresh
        const WinCache& wc = newCache.insert(winCache.extract(f)).position->second;

        std::shared_ptr<AppItem> match = ItemFor(wc);
        if (!match && g_demoMode) continue;
        if (!match) {
            match = NewItem();
            match->aumid = wc.aumid;
            match->exe = wc.exe;
            LoadIconAndName(*match, h);
            items.push_back(match);
            Log(L"running app: %s (aumid %s, package %s)", match->name.c_str(), wc.aumid.c_str(), wc.family.c_str());
            structural = true;
        }
        match->windows.push_back(h);
    }
    winCache.swap(newCache);

    double now = NowSeconds();
    for (size_t i = 0; i < items.size(); ++i) {
        auto& a = items[i];
        if (a->launchStarted > 0) {
            if (a->windows.size() > a->launchWindowCount || now - a->launchStarted > kLaunchBounceMax) a->launchStarted = 0;
        }
    }
    // drop unpinned apps whose windows are gone
    size_t before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(), [this](auto& a) {
        if (a->pinned || a->Running()) return false;
        ForgetPresses(a->id);
        return true;
    }), items.end());
    if (items.size() != before) structural = true;

    // clear attention for the foreground app
    HWND fg = GetForegroundWindow();
    for (auto& a : items)
        if (a->attention && std::find(a->windows.begin(), a->windows.end(), fg) != a->windows.end()) a->attention = false;

    if (structural) structureVersion++;
}

void AppModel::Tick(double now) {
    if (dirty || now - lastRefresh > 1.0) Refresh();
    if (!presses.empty()) PollPresses(now);
    if (!launchChecks.empty()) CheckLaunches(now);
    if (watch.window && now >= watch.next) CheckWatch(now);
}

void AppModel::OnShellHook(WPARAM code, LPARAM lp) {
    HWND h = (HWND)lp;
    switch (code) {
    case HSHELL_WINDOWCREATED:
    case HSHELL_WINDOWDESTROYED:
    case HSHELL_WINDOWREPLACED:
    case HSHELL_MONITORCHANGED:
        dirty = true;
        break;
    case HSHELL_WINDOWACTIVATED:
    case HSHELL_RUDEAPPACTIVATED: {
        if (auto a = FindByWindow(h)) a->attention = false;
        dirty = true;
        break;
    }
    case HSHELL_FLASH: {
        // the dock is bringing this window up and Windows flashed it instead (apps.h, quietFlashes)
        if (FlashQuiet(h, NowSeconds())) break;
        auto a = FindByWindow(h);
        if (!a) { Refresh(); a = FindByWindow(h); }
        if (!a) break;
        if (GetForegroundWindow() == h) break;
        double now = NowSeconds();
        bool fresh = !a->attention;
        a->attention = true;
        // bounce for a new notification; a window that keeps flashing re-bounces at most every 30 s
        if (fresh || now - a->lastAttentionBounce > 30.0) {
            a->lastAttentionBounce = now;
            attentionEvents.push_back({ a->id });
            Log(L"attention: %s", a->name.c_str());
        }
        break;
    }
    }
}
