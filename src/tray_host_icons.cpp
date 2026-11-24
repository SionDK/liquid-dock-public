#include "tray_host_internal.h"

namespace trayhost {

namespace {

// ---------------------------------------------------------------- what Shell_NotifyIcon sends
// The shell's own copy of NOTIFYICONDATAW: handles as 32-bit values, so 32- and 64-bit programs
// send the same bytes (layouts as in ManagedShell / ReactOS).
struct NotifyIconData32 {
    DWORD cbSize; DWORD hWnd; UINT uID; UINT uFlags; UINT uCallbackMessage; DWORD hIcon;
    WCHAR szTip[128]; DWORD dwState; DWORD dwStateMask; WCHAR szInfo[256];
    UINT uVersion;   // with NIM_SETVERSION; uTimeout otherwise
    WCHAR szInfoTitle[64]; DWORD dwInfoFlags; GUID guidItem; DWORD hBalloonIcon;
};
struct TrayNotifyData { DWORD signature; DWORD message; NotifyIconData32 nid; };   // WM_COPYDATA, dwData 1
// Shell_NotifyIconGetRect (WM_COPYDATA, dwData 3): message 1 asks for the top left, 2 the bottom right
struct IconIdentifier { DWORD magic; DWORD message; DWORD cbSize; DWORD padding; DWORD hWnd; UINT uID; GUID guidItem; };
static_assert(sizeof(NotifyIconData32) == 956, "NOTIFYICONDATA32 layout");
static_assert(sizeof(TrayNotifyData) == 964, "TRAYNOTIFYDATA layout");
static_assert(sizeof(IconIdentifier) == 40, "icon identifier layout");
constexpr size_t kMinNotify = offsetof(TrayNotifyData, nid) + offsetof(NotifyIconData32, szTip);

template <class T> T FromHandle32(DWORD v) { return (T)(LONG_PTR)(LONG)v; }   // handles are sign-extended
bool IsNullGuid(const GUID& g) { static const GUID zero{}; return IsEqualGUID(g, zero) != FALSE; }

}   // namespace

std::mutex g_mu;                          // guards g_icons (host thread writes, UI thread reads)
std::vector<Icon> g_icons;
std::atomic<int> g_version{ 0 };

namespace {

uint64_t g_nextKey = 1, g_nextSeq = 0;

struct Picture { int w = 0, h = 0; std::vector<uint32_t> px; bool glyph = false; };

// IsGlyph, Premultiply and Shrink: icon_pixels.h (testable without Windows)
using iconpx::IsGlyph;
using iconpx::Premultiply;
using iconpx::Shrink;

// The program's HICON is only guaranteed during its Shell_NotifyIcon call: copy the pixels now.
bool ReadPicture(HICON icon, Picture& out) {
    out = {};
    if (!icon) return true;   // no picture
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) return false;
    HDC dc = GetDC(nullptr);
    auto bits = [&](HBITMAP b, int w, int h, std::vector<uint32_t>& px) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        px.assign((size_t)w * h, 0);
        return GetDIBits(dc, b, 0, h, px.data(), &bi, DIB_RGB_COLORS) == h;
    };
    bool ok = false;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0 &&
        bm.bmWidth <= 1024 && bm.bmHeight <= 1024) {
        int w = bm.bmWidth, h = bm.bmHeight;
        std::vector<uint32_t> px;
        if (bits(ii.hbmColor, w, h, px)) {
            bool anyAlpha = false;
            for (uint32_t p : px) if (p >> 24) { anyAlpha = true; break; }
            if (!anyAlpha) {   // no alpha channel: the mask tells what is transparent
                std::vector<uint32_t> mask;
                if (ii.hbmMask && bits(ii.hbmMask, w, h, mask))
                    for (size_t i = 0; i < px.size(); ++i) px[i] = (px[i] & 0xffffff) | ((mask[i] & 0xffffff) ? 0 : 0xff000000u);
                else for (auto& p : px) p |= 0xff000000u;
            }
            out.w = w; out.h = h; out.px = std::move(px); ok = true;
        }
    } else if (!ii.hbmColor && ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(bm), &bm) && bm.bmWidth > 0 &&
               bm.bmHeight >= 2 && bm.bmWidth <= 1024 && bm.bmHeight <= 2048) {
        // monochrome: the mask bitmap holds the AND mask above the XOR mask
        int w = bm.bmWidth, h = bm.bmHeight / 2;
        std::vector<uint32_t> m;
        if (bits(ii.hbmMask, w, h * 2, m)) {
            out.px.resize((size_t)w * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    bool andBit = (m[(size_t)y * w + x] & 0xffffff) != 0, xorBit = (m[(size_t)(y + h) * w + x] & 0xffffff) != 0;
                    out.px[(size_t)y * w + x] = andBit ? (xorBit ? 0xff000000u : 0u) : (xorBit ? 0xffffffffu : 0xff000000u);
                }
            out.w = w; out.h = h; ok = true;
        }
    }
    ReleaseDC(nullptr, dc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (!ok) { out = {}; return false; }
    out.glyph = IsGlyph(out.px);
    Premultiply(out.px);
    Shrink(out);
    return true;
}

}   // namespace

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

// process: opened with PROCESS_QUERY_LIMITED_INFORMATION
std::wstring ImagePath(HANDLE process) {
    wchar_t path[MAX_PATH * 2];
    DWORD n = ARRAYSIZE(path);
    std::wstring s;
    if (QueryFullProcessImageNameW(process, 0, path, &n)) s.assign(path, n);
    return s;
}

std::vector<std::pair<HWND, std::wstring>> g_exeCache;   // host thread: the program behind each icon window

namespace {

std::wstring WindowExe(HWND h) {
    for (auto& [w, exe] : g_exeCache) if (w == h) return exe;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!p) return {};
    std::wstring s = ImagePath(p);
    CloseHandle(p);
    s = Lower(s);
    g_exeCache.emplace_back(h, s);
    return s;
}

Icon* Find(bool byGuid, const GUID& guid, HWND hwnd, UINT uid) {
    for (auto& ic : g_icons) {
        if (byGuid) { if (ic.hasGuid && IsEqualGUID(ic.guid, guid)) return &ic; }
        else if (ic.hwnd == hwnd && ic.uid == uid) return &ic;
    }
    return nullptr;
}

// Whether the host has the icon a Shell_NotifyIcon call names.
bool Knows(const NotifyIconData32& n) {
    bool byGuid = (n.uFlags & NIF_GUID) && !IsNullGuid(n.guidItem);
    std::lock_guard<std::mutex> lk(g_mu);
    return Find(byGuid, n.guidItem, FromHandle32<HWND>(n.hWnd), n.uID) != nullptr;
}

// One Shell_NotifyIcon call. True when the host took it (an icon it knows or a new one it added).
bool Apply(DWORD message, const NotifyIconData32& n) {
    HWND hwnd = FromHandle32<HWND>(n.hWnd);
    bool byGuid = (n.uFlags & NIF_GUID) && !IsNullGuid(n.guidItem);
    Picture pic;
    bool wantPic = (message == NIM_ADD || message == NIM_MODIFY) && (n.uFlags & NIF_ICON);
    bool havePic = wantPic && ReadPicture(FromHandle32<HICON>(n.hIcon), pic);
    std::wstring exe = (message == NIM_ADD || message == NIM_MODIFY) && hwnd ? WindowExe(hwnd) : std::wstring();
    bool visibleChange = false, result = false;
    std::vector<std::wstring> lines;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Icon* ic = Find(byGuid, n.guidItem, hwnd, n.uID);
        switch (message) {
        case NIM_ADD:
        case NIM_MODIFY: {
            bool created = false;
            if (!ic) {
                if (!hwnd || !IsWindow(hwnd)) return false;
                g_icons.emplace_back();
                ic = &g_icons.back();
                created = true;
                ic->key = g_nextKey++; ic->seq = g_nextSeq++;
                ic->hwnd = hwnd; ic->uid = n.uID; ic->exe = exe;
                if (byGuid) { ic->hasGuid = true; ic->guid = n.guidItem; }
            }
            if (hwnd && hwnd != ic->hwnd) { ic->hwnd = hwnd; ic->exe = exe; }   // an icon by GUID can move to a new window
            // an icon added again under its GUID answers to the new id: a program re-making its icon
            // (Electron numbers every tray icon it makes, and ignores clicks for any other number)
            if (!byGuid || message == NIM_ADD) ic->uid = n.uID;
            if (n.uFlags & NIF_MESSAGE) ic->callback = n.uCallbackMessage;
            if (n.uFlags & NIF_TIP) {
                std::wstring tip(n.szTip, wcsnlen(n.szTip, ARRAYSIZE(n.szTip)));
                if (tip != ic->tip) { ic->tip = std::move(tip); visibleChange = true; }
            }
            bool showTip = (n.uFlags & NIF_SHOWTIP) != 0;
            if (showTip || message == NIM_ADD || (n.uFlags & NIF_TIP)) {
                if (showTip != ic->showTip) { ic->showTip = showTip; visibleChange = true; }
            }
            if ((n.uFlags & NIF_STATE) && (n.dwStateMask & NIS_HIDDEN)) {
                bool hidden = (n.dwState & NIS_HIDDEN) != 0;
                if (hidden != ic->hidden) { ic->hidden = hidden; visibleChange = true; }
            }
            if (havePic && (pic.w != ic->w || pic.h != ic->h || pic.px != ic->pixels)) {
                ic->w = pic.w; ic->h = pic.h; ic->pixels = std::move(pic.px); ic->glyph = pic.glyph;
                ic->imageVersion++;
                visibleChange = true;
            }
            if (created) {
                UpdatePromotion(*ic);
                visibleChange = true;
                wchar_t size[32];
                swprintf_s(size, L"%dx%d", ic->w, ic->h);
                DescribeIcon(*ic, message == NIM_ADD ? L"+" : L"+ (by a modify)", !wantPic ? L"not sent" : !havePic ? L"unreadable" : ic->w ? size : L"empty",
                             lines);
            }
            else if (visibleChange && !byGuid) UpdatePromotion(*ic);   // hidden siblings may change the match
            result = !created || message == NIM_ADD;   // a modify for an unknown icon: Explorer's answer counts
            break;
        }
        case NIM_DELETE:
            if (ic) {
                g_icons.erase(g_icons.begin() + (ic - g_icons.data()));
                visibleChange = result = true;
            }
            break;
        case NIM_SETVERSION:
            if (ic && n.uVersion <= 4) {
                if (ic->version != n.uVersion) { ic->version = n.uVersion; visibleChange = true; }   // tooltips depend on it
                result = true;
            }
            break;
        case NIM_SETFOCUS:
            result = ic != nullptr;
            break;
        }
    }
    LogLines(lines);
    if (visibleChange) g_version++;
    return result;
}

bool AnswerIconRect(const IconIdentifier& id, LRESULT& out) {
    if (id.message != 1 && id.message != 2) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    HWND hwnd = FromHandle32<HWND>(id.hWnd);
    for (const auto& ic : g_icons) {
        bool same = (!IsNullGuid(id.guidItem) && ic.hasGuid && IsEqualGUID(ic.guid, id.guidItem)) ||
                    (ic.hwnd == hwnd && ic.uid == id.uID);
        if (!same) continue;
        if (!ic.hasAnchor || !ic.promoted || ic.hidden) return false;   // shown by Explorer: it knows where
        const RECT& r = ic.anchor;
        LONG x = id.message == 1 ? r.left : r.right, y = id.message == 1 ? r.top : r.bottom;
        out = (LRESULT)(DWORD)MAKELONG((WORD)(SHORT)x, (WORD)(SHORT)y);
        return true;
    }
    return false;
}

}   // namespace

LRESULT OnCopyData(WPARAM wp, LPARAM lp) {
    const auto* cds = (const COPYDATASTRUCT*)lp;
    LRESULT r = 0;
    if (cds && cds->dwData == 1 && cds->lpData && cds->cbData >= kMinNotify) {
        TrayNotifyData d{};
        memcpy(&d, cds->lpData, std::min<size_t>(cds->cbData, sizeof(d)));
        if (d.message == NIM_MODIFY && !Knows(d.nid)) {
            // A modify for an icon the host does not have: it missed the add (it started later), or the
            // program deleted the icon and still updates it (a timer). Explorer's answer tells which, so
            // it is asked first: an icon Explorer has not got never comes up on the dock, not even for a
            // moment. (The program waits in Shell_NotifyIcon meanwhile: its picture stays readable.)
            bool passed = Forward(WM_COPYDATA, wp, lp, r);
            if (!passed || r) Apply(d.message, d.nid);   // Explorer has it, or cannot say: as before
            return passed ? r : FALSE;
        }
        bool mine = Apply(d.message, d.nid);
        bool passed = Forward(WM_COPYDATA, wp, lp, r);
        // NIM_ADD: Explorer refuses an icon it has already (after the re-registration this host asks
        // for at start), while the program still has to see success
        if (d.message == NIM_ADD) return (mine || (passed && r)) ? TRUE : FALSE;
        return passed ? r : (mine ? TRUE : FALSE);
    }
    if (cds && cds->dwData == 3 && cds->lpData && cds->cbData >= sizeof(IconIdentifier)) {
        IconIdentifier id{};
        memcpy(&id, cds->lpData, sizeof(id));
        if (AnswerIconRect(id, r)) return r;
    }
    if (cds && cds->dwData == 0) ForwardAppBar(wp, lp, r);   // app bars: Explorer's business
    else Forward(WM_COPYDATA, wp, lp, r);                   // and everything else
    return r;
}

int Version() { return g_version.load(); }

std::vector<IconView> Shown() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<const Icon*> shown;
    for (const auto& ic : g_icons)
        if (ShownOnDock(ic)) shown.push_back(&ic);
    std::stable_sort(shown.begin(), shown.end(), [](const Icon* a, const Icon* b) {
        return a->order != b->order ? a->order < b->order : a->seq < b->seq;
    });
    std::vector<IconView> out;
    out.reserve(shown.size());
    for (const Icon* ic : shown) {
        IconView v;
        v.key = ic->key;
        // version 4 programs draw their own pop-up unless they ask for the standard tooltip
        if (ic->version < 4 || ic->showTip) v.tip = ic->tip;
        v.imageVersion = ic->imageVersion;
        v.glyph = ic->glyph;
        out.push_back(std::move(v));
    }
    return out;
}

bool Image(uint64_t key, uint32_t& imageVersion, int& w, int& h, std::vector<uint32_t>& pixels) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& ic : g_icons) {
        if (ic.key != key) continue;
        imageVersion = ic.imageVersion; w = ic.w; h = ic.h; pixels = ic.pixels;
        return true;
    }
    return false;
}

void Send(uint64_t key, Mouse m, POINT cursor, const RECT& anchor) {
    HWND hwnd = nullptr;
    UINT callback = 0, uid = 0, version = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& ic : g_icons) {
            if (ic.key != key) continue;
            ic.hasAnchor = true; ic.anchor = anchor;
            hwnd = ic.hwnd; callback = ic.callback; uid = ic.uid; version = ic.version;
            break;
        }
    }
    if (!hwnd) return;
    if (!IsWindow(hwnd)) {   // the program died without removing it: Explorer drops such icons too
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (size_t i = 0; i < g_icons.size(); ++i)
                if (g_icons[i].key == key) { g_icons.erase(g_icons.begin() + i); break; }
        }
        g_version++;
        return;
    }
    if (!callback) return;
    bool click = m == Mouse::LeftClick || m == Mouse::LeftDoubleClick || m == Mouse::RightClick || m == Mouse::MiddleClick;
    // As administrator this process gets past the barrier that keeps Explorer's clicks away from elevated
    // windows, while any program may register an icon naming any window. Pass on only what a tray icon is
    // for: a program-defined message (WM_USER and up) to a top-level or message-only window - one that is
    // its own root, whatever its style (ATL gives message-only windows WS_CHILD), not another's control.
    if (g_elevated && (callback < WM_USER || GetAncestor(hwnd, GA_ROOT) != hwnd)) {
        if (click) Log(L"tray host: click not passed on (%s)", callback < WM_USER ? L"a system message as the callback" : L"a child window");
        return;
    }
    if (click) {
        // its menu or window may take the foreground (this process got the click, so it may allow that)
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid) AllowSetForegroundWindow(pid);
    }
    // version 4: wParam = the pointer, lParam = event | icon id << 16; before: wParam = id, lParam = event
    auto notify = [&](UINT event) {
        WPARAM w = version > 3 ? (WPARAM)MAKELONG((WORD)(SHORT)cursor.x, (WORD)(SHORT)cursor.y) : (WPARAM)uid;
        LPARAM l = version > 3 ? (LPARAM)MAKELONG((WORD)event, (WORD)uid) : (LPARAM)event;
        SendNotifyMessageW(hwnd, callback, w, l);   // never waits on a hung program
    };
    switch (m) {
    case Mouse::LeftClick:       notify(WM_LBUTTONDOWN); notify(WM_LBUTTONUP); if (version >= 3) notify(NIN_SELECT); break;
    case Mouse::LeftDoubleClick: notify(WM_LBUTTONDBLCLK); notify(WM_LBUTTONUP); break;
    case Mouse::RightClick:      notify(WM_RBUTTONDOWN); notify(WM_RBUTTONUP); if (version >= 3) notify(WM_CONTEXTMENU); break;
    case Mouse::MiddleClick:     notify(WM_MBUTTONDOWN); notify(WM_MBUTTONUP); break;
    case Mouse::Enter:           notify(WM_MOUSEHOVER); if (version > 3) notify(NIN_POPUPOPEN); break;
    case Mouse::Move:            notify(WM_MOUSEMOVE); break;
    case Mouse::Leave:           notify(WM_MOUSELEAVE); if (version > 3) notify(NIN_POPUPCLOSE); break;
    }
}

}   // namespace trayhost
