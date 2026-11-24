#include "dock.h"
#include <d2d1effects.h>

// ---------------------------------------------------------------- tray icons (Settings::trayIcons)
// The icons Windows shows on its own taskbar, as the tray host (tray_host.h) receives them. They sit
// between the hidden-icons chevron and network / volume: like the small buttons, two share a cell
// across the bar's thickness while half of it is still a comfortable target.

namespace {
struct NotifyBitmap { uint32_t imageVersion = 0; ComPtr<ID2D1Bitmap1> bmp; ComPtr<ID2D1Effect> tint; int w = 0, h = 0; };
std::unordered_map<uint64_t, NotifyBitmap> g_notifyBitmaps;   // shared by the docks (one device context)

// The picture of an icon, made once per change of the picture.
NotifyBitmap* BitmapFor(uint64_t key, uint32_t imageVersion) {
    auto it = g_notifyBitmaps.find(key);
    if (it != g_notifyBitmaps.end() && it->second.bmp && it->second.imageVersion >= imageVersion) return &it->second;
    uint32_t ver = 0; int w = 0, h = 0;
    std::vector<uint32_t> px;
    if (!trayhost::Image(key, ver, w, h, px) || w <= 0 || h <= 0) return nullptr;
    HRESULT hr = S_OK;
    ComPtr<ID2D1Bitmap1> b = BitmapFromPixels(g_gfx.dc, px, w, h, &hr);
    if (FAILED(hr)) return nullptr;
    auto& e = g_notifyBitmaps[key];
    e.bmp = b; e.imageVersion = ver; e.w = w; e.h = h; e.tint.Reset();
    return &e;
}

// tooltips are one line on the dock
std::wstring OneLine(std::wstring s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'\r') continue;
        if (c == L'\n') { if (!out.empty() && out.back() != L' ') out += L" · "; continue; }
        out += c;
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'·')) out.pop_back();
    return out;
}
}

void Dock::ResetNotifyBitmaps() { g_notifyBitmaps.clear(); }

void Dock::SyncNotifyIcons(double now) {
    // the host may run for FluentFlyout's widget alone: icons only with the setting on
    int v = g_settings.trayIcons ? trayhost::Version() * 2 + 1 : 0;
    if (v == notifyVersion) return;
    // an always-shown dock at rest (or during a game elsewhere) takes icon changes twice a second at most:
    // some programs animate their tray icon
    if (restingPrev && now < nextNotifySync) return;
    nextNotifySync = now + 0.5;
    notifyVersion = v;
    auto views = g_settings.trayIcons ? trayhost::Shown() : std::vector<trayhost::IconView>{};
    uint64_t hovered = NotifyKey(hover.notify);
    std::vector<NotifyCell> fresh;
    fresh.reserve(views.size());
    for (auto& iv : views) {
        NotifyCell c;
        c.key = iv.key; c.tip = OneLine(iv.tip); c.imageVersion = iv.imageVersion; c.glyph = iv.glyph;
        fresh.push_back(std::move(c));
    }
    notifyCells = std::move(fresh);
    hover.notify = -1;
    for (int i = 0; i < (int)notifyCells.size(); ++i) if (notifyCells[i].key == hovered) hover.notify = i;
    for (auto it = g_notifyBitmaps.begin(); it != g_notifyBitmaps.end();) {   // pictures of icons that are gone
        bool used = std::any_of(views.begin(), views.end(), [&](const trayhost::IconView& iv) { return iv.key == it->first; });
        it = used ? std::next(it) : g_notifyBitmaps.erase(it);
    }
    dirty = true;
}

float Dock::ArrangeNotify(const LayoutMetrics& m, float x, bool assign) {
    const size_t n = notifyCells.size();
    if (!n) return x;
    const float w = std::max(m.icon * 0.55f, 24 * scale);   // the small buttons' cell
    const size_t cols = m.stack ? (n + 1) / 2 : n;
    if (assign)
        for (size_t i = 0; i < n; ++i) {
            auto& c = notifyCells[i];
            size_t col = m.stack ? i / 2 : i;
            c.u0 = x + col * w;
            c.u1 = c.u0 + w;
            c.half = (m.stack && !(i + 1 == n && n % 2 == 1)) ? (int)(i % 2) : -1;   // an odd last one keeps a whole cell
        }
    return x + cols * w;
}

D2D1_RECT_F Dock::NotifyRect(int i) const {
    if (i < 0 || i >= (int)notifyCells.size()) return {};
    const auto& c = notifyCells[i];
    if (c.u1 <= c.u0) return {};
    auto r = RectUV(c.u0, c.u1, BarV0(), BarV1());
    SplitHalf(r, c.half);   // as the small buttons: the first one left (side docks) or on top
    return r;
}

RECT Dock::NotifyScreenRect(int i) const {
    return ScreenRectOut(NotifyRect(i));
}

void Dock::DrawNotifyIcons(float vis) {
    auto* dc = g_gfx.dc.Get();
    for (int i = 0; i < (int)notifyCells.size(); ++i) {
        const auto& c = notifyCells[i];
        NotifyBitmap* b = BitmapFor(c.key, c.imageVersion);
        if (!b) continue;
        D2D1_RECT_F cell = NotifyRect(i);
        float side = std::min(cell.right - cell.left, cell.bottom - cell.top);
        float target = side * 0.74f, native = (float)std::max(b->w, b->h);
        float s = std::fabs(native - target) <= target * 0.3f ? native : target;   // the icon's own size when close: crisp
        float sw = s * b->w / native, sh = s * b->h / native;
        float x = std::round((cell.left + cell.right - sw) * 0.5f), y = std::round((cell.top + cell.bottom - sh) * 0.5f);
        if (c.glyph) {   // a white or black glyph made for the taskbar's theme: drawn in the dock's ink, like its own icons
            if (!b->tint && SUCCEEDED(dc->CreateEffect(CLSID_D2D1ColorMatrix, &b->tint))) b->tint->SetInput(0, b->bmp.Get());
            if (b->tint) {
                auto ink = Ink(c.Mid(), vis);
                D2D1_MATRIX_5X4_F mtx = { 0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, ink.a,  ink.r, ink.g, ink.b, 0 };
                b->tint->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, mtx);
                dc->SetTransform(D2D1::Matrix3x2F::Scale(sw / b->w, sh / b->h) * D2D1::Matrix3x2F::Translation(x, y));
                dc->DrawImage(b->tint.Get(), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                dc->SetTransform(D2D1::Matrix3x2F::Identity());
                continue;
            }
        }
        dc->DrawBitmap(b->bmp.Get(), { x, y, x + sw, y + sh }, vis, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    }
}

// Like Explorer: the program hears the pointer move over its icon right away, and after a moment that
// it may open its own pop-up (version 4 programs) - and that it can close it when the pointer leaves.
void Dock::NotifyHover(double now, POINT cursor) {
    uint64_t key = hover.kind == HitNotify ? NotifyKey(hover.notify) : 0;
    if (key != notifyHoverKey) {
        if (notifyHoverKey && notifyEntered) trayhost::Send(notifyHoverKey, trayhost::Mouse::Leave, cursor, notifyHoverAnchor);
        notifyHoverKey = key; notifyHoverSince = now; notifyEntered = false; nextNotifyMove = 0;
    }
    if (!key) return;
    notifyHoverAnchor = NotifyScreenRect(hover.notify);
    if (now >= nextNotifyMove && (nextNotifyMove == 0 || cursor.x != lastNotifyCursor.x || cursor.y != lastNotifyCursor.y)) {
        trayhost::Send(key, trayhost::Mouse::Move, cursor, notifyHoverAnchor);
        nextNotifyMove = now + 0.05; lastNotifyCursor = cursor;
    }
    if (!notifyEntered && now - notifyHoverSince >= 0.4) {
        trayhost::Send(key, trayhost::Mouse::Enter, cursor, notifyHoverAnchor);
        notifyEntered = true;
    }
}

void Dock::NotifyClick(int i, trayhost::Mouse m) {
    uint64_t key = NotifyKey(i);
    if (!key) return;
    POINT p{}; GetCursorPos(&p);
    if (m == trayhost::Mouse::LeftClick) {   // the dock's window takes no double clicks: tell them apart here
        DWORD t = GetTickCount();
        bool dbl = key == lastNotifyClickKey && t - lastNotifyClickTick <= GetDoubleClickTime() &&
                   std::abs(p.x - lastNotifyClickPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                   std::abs(p.y - lastNotifyClickPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        if (dbl) m = trayhost::Mouse::LeftDoubleClick;
        lastNotifyClickKey = dbl ? 0 : key; lastNotifyClickTick = t; lastNotifyClickPt = p;
    }
    trayhost::Send(key, m, p, NotifyScreenRect(i));
}
