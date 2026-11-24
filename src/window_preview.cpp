#include "window_preview.h"

namespace {
constexpr double kHoverDelay = 0.45, kLeaveDelay = 0.25;
// After a card's close: its close button rests this long while the window goes (a second press would
// ask a program that is asking to save once more); a window still there by then can be asked again.
constexpr double kCloseAgain = 2.0;
// After a close, a new front window is the program's doing (the next window, or its "save?" question),
// not the user switching away: the popup stays with the windows left.
constexpr double kCloseGrace = 1.5;
constexpr BYTE kClosingOpacity = 110;   // the image of a window asked to close
constexpr wchar_t kClass[] = L"LiquidDockWindowPreview";
int Px(float value, float scale) { return std::max(1, (int)std::lround(value * scale)); }
bool SameRect(RECT a, RECT b) { return EqualRect(&a, &b) != FALSE; }
// Lets go of a card's DWM relationship, if it holds one.
void Unthumb(HTHUMBNAIL& thumbnail) {
    if (thumbnail) { DwmUnregisterThumbnail(thumbnail); thumbnail = nullptr; }
}
void Fill(HDC dc, RECT r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &r, brush); DeleteObject(brush);
}
bool Usable(HWND source) {
    if (!IsWindow(source) || !IsWindowVisible(source)) return false;
    DWORD cloaked = 0;
    return FAILED(DwmGetWindowAttribute(source, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || !cloaked;
}
// A minimized window's preview is the image DWM kept of it when it was minimized, the one
// the taskbar shows; it does not change while the window stays minimized. For a window that
// has not been on screen since it was minimized (one started minimized), DWM makes a card of
// its own instead - its icon on a grey ground (300x188 at 125 %, Windows 11 25H2), what the
// taskbar shows too - and that is shown. The placeholder only when DWM gives no size, or about
// the minimized window itself (its caption, off screen: 237x39 at 125 % on 25H2; give or take
// borders and DPI rounding, so anything less than twice the caption's height is taken for it).
constexpr LONG kKeptImageMin = 64;   // px a side
bool KeptImage(HWND source, SIZE size) {
    if (size.cx < kKeptImageMin || size.cy < kKeptImageMin) return false;
    RECT own{};
    return !GetWindowRect(source, &own) || size.cy >= 2 * (own.bottom - own.top);
}
enum class Kept : uint8_t { Shown, NotShown, NoImage, NoAnswer };
// One dock.log line per minimized window and outcome, however often its preview opens: what
// DWM reported is what chose between its image and the placeholder. No titles.
void NoteMinimized(HWND source, const std::wstring& app, Kept what, HRESULT hr, SIZE size) {
    struct Said { Kept what; HRESULT hr; LONG cx, cy; };
    static std::unordered_map<HWND, Said> said;   // the UI thread's
    auto it = said.find(source);
    if (it != said.end() && it->second.what == what && it->second.hr == hr && it->second.cx == size.cx &&
        it->second.cy == size.cy) return;
    if (said.size() >= 256) said.clear();
    said[source] = {what, hr, size.cx, size.cy};
    wchar_t cls[64]{}; GetClassNameW(source, cls, ARRAYSIZE(cls));
    RECT own{}; GetWindowRect(source, &own);
    switch (what) {
    case Kept::Shown:
        Log(L"preview: minimized window of %ls (%ls) - DWM gives an image of %ldx%ld: shown", app.c_str(), cls, size.cx, size.cy);
        break;
    case Kept::NotShown:
        Log(L"preview: minimized window of %ls (%ls) - DWM gives an image of %ldx%ld but did not show it (0x%08lx): placeholder",
            app.c_str(), cls, size.cx, size.cy, (unsigned long)hr);
        break;
    case Kept::NoAnswer:
        Log(L"preview: minimized window of %ls (%ls) - DWM gave no image (0x%08lx): placeholder", app.c_str(), cls, (unsigned long)hr);
        break;
    case Kept::NoImage:
        Log(L"preview: minimized window of %ls (%ls) - no image kept (DWM reports %ldx%ld, the window is %ldx%ld): placeholder",
            app.c_str(), cls, size.cx, size.cy, own.right - own.left, own.bottom - own.top);
        break;
    }
}
}

WindowPreview::~WindowPreview() {
    Hide();
    if (hwnd) DestroyWindow(hwnd);
    if (font) DeleteObject(font);
    if (iconFont) DeleteObject(iconFont);
}

bool WindowPreview::Create(HWND owner) {
    if (hwnd) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kClass, L"열린 창 미리보기", WS_POPUP, 0, 0, 1, 1, owner, nullptr, wc.hInstance, this);
    if (!hwnd) return false;
    // Keep the popup out of the dock's background capture, like the dock itself.
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    return true;
}

void WindowPreview::ReleaseThumbnails() {
    for (auto& e : entries) Unthumb(e.thumbnail);
}

void WindowPreview::Hide() {
    // Nothing shown and nothing held: the dock comes by on every pass while no preview may open (the dock
    // hidden, a fullscreen app). Then no ShowWindow, a call into the kernel each time; IsWindowVisible and
    // GetCapture are answered inside the process.
    if (!app && entries.empty() && active != this && (!hwnd || (!IsWindowVisible(hwnd) && GetCapture() != hwnd))) return;
    if (hwnd) ShowWindow(hwnd, SW_HIDE);
    ReleaseThumbnails(); entries.clear(); app.reset();
    first = 0; hot = -1; hotClose = -1; pressed = nullptr; pressedClose = nullptr; pressedPid = 0; pressedNav = 0;
    middle = nullptr; middlePid = 0; closeSent = false; closeGrace = -1;
    outsideSince = -1; wheel = 0;
    if (active == this) active = nullptr;
    if (hwnd && GetCapture() == hwnd) ReleaseCapture();
}

void WindowPreview::Dismiss() {
    Hide(); suppressed = true; // require leaving the icon before reopening
}

bool WindowPreview::Contains(POINT p) const {
    return Open() && (PtInRect(&bounds, p) || PtInRect(&bridge, p) || PtInRect(&anchor, p));
}

bool WindowPreview::Open() const {
    return app != nullptr && hwnd && IsWindowVisible(hwnd);
}

bool WindowPreview::Valid(const Entry& e) const {
    DWORD pid = 0;
    GetWindowThreadProcessId(e.source, &pid);
    return app && pid && pid == e.pid && Usable(e.source) &&
        std::find(app->windows.begin(), app->windows.end(), e.source) != app->windows.end();
}

void WindowPreview::Update(HWND owner, const std::shared_ptr<AppItem>& hovered, RECT newAnchor,
    RECT newMonitor, Edge newEdge, float newScale, POINT cursor, double now, bool allowed) {
    // Not allowed (the dock coming up or going, a press, a menu, a drag, a fullscreen app): no preview
    // meanwhile. Only a popup put away then waits for the pointer to leave its icon; one that had not
    // opened yet opens once allowed, as for any hover (a press dismisses it in the dock's own handler).
    if (!allowed) {
        if (Open()) Dismiss(); else Hide();
        candidate = hovered; since = now; return;
    }
    if (candidate != hovered) {
        candidate = hovered; since = now; suppressed = false;
        if (hovered && app != hovered) Hide();
    }
    if (Open()) {
        if (closeSent) { closeSent = false; closeGrace = now + kCloseGrace; }
        bool rested = false;   // a window asked to close is still there after kCloseAgain: its button works again
        for (auto& e : entries) {
            if (!e.closing) continue;
            if (e.closingSince < 0) e.closingSince = now;
            else if (now - e.closingSince >= kCloseAgain) { e.closing = false; e.closingSince = -1; rested = true; }
        }
        HWND front = GetForegroundWindow();
        if (front != foreground && now < closeGrace) foreground = front;   // what a close brought to the front
        bool outsideClick = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) &&
                            !PtInRect(&bounds, cursor);
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || outsideClick || front != foreground) {
            Dismiss(); return;
        }
        if (rested) {
            Arrange();
            if (!app) return;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (hovered == app && !SameRect(anchor, newAnchor)) {
            anchor = newAnchor; Arrange(true);
            if (!app) return;
        }
        if (Contains(cursor) || hovered == app) outsideSince = -1;
        else if (outsideSince < 0) outsideSince = now;
        else if (now - outsideSince >= kLeaveDelay) { Hide(); return; }
        if (now >= nextRefresh) {
            nextRefresh = now + 0.2; Refresh();
            if (entries.empty()) { Dismiss(); return; }
        }
        POINT client{cursor.x - bounds.left, cursor.y - bounds.top};
        int nextHot = Hit(client), nextHotClose = HitClose(client);
        if (nextHot != hot || nextHotClose != hotClose) {
            hot = nextHot; hotClose = nextHotClose; InvalidateRect(hwnd, nullptr, FALSE);
        }
        return;
    }
    if (!hovered || hovered->windows.empty() || suppressed || now - since < kHoverDelay) return;
    if (!Create(owner)) { suppressed = true; return; }
    if (active && active != this) active->Dismiss();
    active = this; app = hovered; anchor = newAnchor; monitor = newMonitor; edge = newEdge;
    float fontScale = std::clamp(newScale, 0.72f, 2.5f);
    if (!font || scale != fontScale) {
        if (font) DeleteObject(font);
        if (iconFont) DeleteObject(iconFont);
        scale = fontScale;
        font = CreateFontW(-Px(13, scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        // the close button's X: Windows' own glyph (Windows 10 and 11), as on title bars and the taskbar
        iconFont = CreateFontW(-Px(10, scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe MDL2 Assets");
    }
    foreground = GetForegroundWindow(); nextRefresh = now + 0.2;
    Refresh();
    if (entries.empty()) { Dismiss(); return; }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

void WindowPreview::Refresh() {
    if (!app) return;
    // Preserve order while the popup is open; z-order changes must not move a
    // different window under a pointer that is already over a card.
    bool changed = false;
    for (auto it = entries.begin(); it != entries.end();) {
        if (!Valid(*it)) {
            Unthumb(it->thumbnail);
            it = entries.erase(it); changed = true;
        } else ++it;
    }
    for (HWND source : app->windows) {
        if (!Usable(source) || std::any_of(entries.begin(), entries.end(),
            [&](const Entry& e) { return e.source == source; })) continue;
        Entry e; e.source = source; GetWindowThreadProcessId(source, &e.pid);
        if (e.pid) { entries.push_back(std::move(e)); changed = true; }
    }
    for (auto& e : entries) {
        wchar_t text[512]{}; GetWindowTextW(e.source, text, ARRAYSIZE(text));
        std::wstring title = *text ? text : app->name;
        if (title != e.title) { e.title = std::move(title); changed = true; }
    }
    // Also refresh minimized/protection state and thumbnail size without
    // reading source pixels or requesting the source app to render.
    Arrange();
    if (changed) InvalidateRect(hwnd, nullptr, FALSE);
}

// follow: only the anchor changed - the icon under the open popup grows or slides with the magnification,
// every frame while the pointer moves on it. A popup of the same size and page then only moves along: its
// cards lie in its own coordinates and DWM draws their pictures with it, so neither they nor their windows
// are asked about again (the 0.2 s Refresh looks at the windows, as while the icon stays still).
void WindowPreview::Arrange(bool follow) {
    if (entries.empty()) return;
    const int wasCapacity = capacity, wasFirst = first;
    const Metrics m = Measure();
    const int pad = m.pad, header = m.header, nav = m.nav;
    Placement at;
    if (!PlacePopup(m, at)) {   // no room for it next to the icon
        Hide(); suppressed = true; return;
    }
    const int x = at.x, y = at.y, w = at.w, h = at.h;
    RECT nextBounds{x, y, x + w, y + h};
    bool moved = !SameRect(bounds, nextBounds);
    // the same size, page and cards a page: every card where it was (count, cw and ch follow from them)
    const bool sameCards = w == bounds.right - bounds.left && h == bounds.bottom - bounds.top &&
                           capacity == wasCapacity && first == wasFirst;
    bounds = nextBounds;
    bridge = anchor;
    if (edge == Edge::Left) bridge.right = bounds.left;
    if (edge == Edge::Right) bridge.left = bounds.right;
    if (edge == Edge::Top) bridge.bottom = bounds.top;
    if (edge == Edge::Bottom) bridge.top = bounds.bottom;
    // Small padding avoids a dead seam at either end of the crossing.
    InflateRect(&bridge, 2, 2);
    next = {w - pad - nav, pad, w - pad, pad + header};
    previous = {next.left - nav, pad, next.left, pad + header};
    if (follow && sameCards) {   // a move: the window's region and picture stay as they are
        if (moved && !SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE)) Dismiss();
        return;
    }
    PlaceCards(m, at, moved);
    if (moved) {
        if (!SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE)) {
            Dismiss(); return;
        }
        HRGN rounded = CreateRoundRectRgn(0, 0, w + 1, h + 1, Px(14, scale), Px(14, scale));
        if (!SetWindowRgn(hwnd, rounded, TRUE)) DeleteObject(rounded);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

WindowPreview::Metrics WindowPreview::Measure() const {
    return {Px(8, scale), Px(8, scale), Px(30, scale), Px(28, scale)};
}

// Arrange's popup: its place next to the icon on the monitor, its size and the page it shows (capacity and first
// are set). false: no room for it there, and nothing changed.
bool WindowPreview::PlacePopup(const Metrics& m, Placement& at) {
    const int pad = m.pad, gap = m.gap, header = m.header;
    const bool vertical = IsVertical(edge);
    RECT available = monitor;
    InflateRect(&available, -pad, -pad);
    switch (edge) {
    case Edge::Left: available.left = std::max(available.left, anchor.right + gap); break;
    case Edge::Right: available.right = std::min(available.right, anchor.left - gap); break;
    case Edge::Top: available.top = std::max(available.top, anchor.bottom + gap); break;
    case Edge::Bottom: available.bottom = std::min(available.bottom, anchor.top - gap); break;
    }
    int aw = available.right - available.left, ah = available.bottom - available.top;
    // Do not overlap the dock to make a preview fit on an unusably small area.
    if (aw < 2 * pad + Px(80, scale) || ah < header + 2 * pad + Px(60, scale)) return false;
    int cw = std::min(Px(220, scale), aw - 2 * pad);
    int ch = std::min(Px(156, scale), ah - header - 2 * pad);
    capacity = std::max(1, vertical ? (ah - header - pad) / (ch + pad) : (aw - pad) / (cw + pad));
    capacity = std::min(capacity, (int)entries.size());
    first = std::min(first / capacity, ((int)entries.size() - 1) / capacity) * capacity;
    int count = std::min(capacity, (int)entries.size() - first);
    int w = 2 * pad + (vertical ? cw : count * cw + (count - 1) * pad);
    int h = header + 2 * pad + (vertical ? count * ch + (count - 1) * pad : ch);
    int x = (anchor.left + anchor.right - w) / 2, y = (anchor.top + anchor.bottom - h) / 2;
    if (edge == Edge::Left) x = available.left;
    if (edge == Edge::Right) x = available.right - w;
    if (edge == Edge::Top) y = available.top;
    if (edge == Edge::Bottom) y = available.bottom - h;
    x = std::clamp<int>(x, available.left, available.right - w);
    y = std::clamp<int>(y, available.top, available.bottom - h);
    at = {vertical, cw, ch, count, x, y, w, h};
    return true;
}

// Arrange's cards: each card of the page shown, in the popup's own coordinates, with its window's picture or the
// words in its place; the cards of other pages let go of theirs. moved: set when a picture's place or words changed.
void WindowPreview::PlaceCards(const Metrics& m, const Placement& at, bool& moved) {
    const int pad = m.pad, header = m.header;
    const bool vertical = at.vertical;
    const int cw = at.cw, ch = at.ch, count = at.count;
    for (int i = 0; i < (int)entries.size(); ++i) {
        auto& e = entries[i];
        // Minimized windows show the image DWM kept of them, like the taskbar's previews
        // (KeptImage); only without one the placeholder. Nothing restores the window.
        const bool minimized = IsIconic(e.source) != FALSE;
        if (!minimized) e.noImage = false;   // shown again since: asked anew, on whatever page
        if (i < first || i >= first + count) {
            Unthumb(e.thumbnail);
            e.card = {}; continue;
        }
        int j = i - first;
        int cx = pad + (vertical ? 0 : j * (cw + pad));
        int cy = pad + header + (vertical ? j * (ch + pad) : 0);
        e.card = {cx, cy, cx + cw, cy + ch};
        RECT image{cx + pad, cy + header, cx + cw - pad, cy + ch - pad};
        const wchar_t* none = minimized ? L"최소화된 창 · 클릭하여 복원" : L"미리보기 없음 · 클릭하여 열기";
        std::wstring fallback;
        DWORD affinity = WDA_NONE;
        if (GetWindowDisplayAffinity(e.source, &affinity) && affinity != WDA_NONE)
            fallback = minimized ? none : L"미리보기가 제한된 창";
        else if (e.noImage) fallback = none;   // no new image comes while it stays minimized: not asked again
        if (!fallback.empty()) Unthumb(e.thumbnail);
        else ShowThumbnail(e, image, minimized, none, fallback);
        if (fallback != e.fallback || !SameRect(image, e.image)) moved = true;
        e.image = image; e.fallback = std::move(fallback);
    }
}

// A card's DWM thumbnail fitted into its picture's place (image): registered when it holds none, its properties sent
// when they change, a minimized window's outcome noted. Without a picture to show, fallback gets the words (none)
// and the relationship is let go.
void WindowPreview::ShowThumbnail(Entry& e, const RECT& image, bool minimized, const wchar_t* none, std::wstring& fallback) {
    HRESULT hr = S_OK;
    if (!e.thumbnail) {
        e.sentDestination = {};   // a new relationship: DWM has no properties for it yet
        if (FAILED(hr = DwmRegisterThumbnail(hwnd, e.source, &e.thumbnail))) e.thumbnail = nullptr;
    }
    SIZE size{};
    if (e.thumbnail && FAILED(hr = DwmQueryThumbnailSourceSize(e.thumbnail, &size))) size = {};
    bool usable = e.thumbnail && size.cx > 0 && size.cy > 0 && (!minimized || KeptImage(e.source, size));
    // Only DWM's own answer that it has no image is kept; a failed call is tried again, as for a live window.
    if (minimized) e.noImage = e.thumbnail && SUCCEEDED(hr) && !usable;
    HRESULT drawn = S_OK;
    if (!usable) fallback = none;
    else {
        double fit = std::min(double(image.right - image.left) / size.cx, double(image.bottom - image.top) / size.cy);
        int iw = std::max(1, (int)(size.cx * fit)), ih = std::max(1, (int)(size.cy * fit));
        int ix = (image.left + image.right - iw) / 2, iy = (image.top + image.bottom - ih) / 2;
        const RECT destination{ix, iy, ix + iw, iy + ih};
        const BYTE opacity = e.closing ? kClosingOpacity : 255;
        // DWM keeps a thumbnail's properties: sent when they change, not again on every Refresh
        if (!SameRect(destination, e.sentDestination) || opacity != e.sentOpacity) {
            DWM_THUMBNAIL_PROPERTIES props{};
            props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            props.rcDestination = destination;
            props.fVisible = TRUE; props.opacity = opacity; props.fSourceClientAreaOnly = FALSE;
            if (FAILED(drawn = DwmUpdateThumbnailProperties(e.thumbnail, &props))) fallback = none;
            else { e.sentDestination = destination; e.sentOpacity = opacity; }
        }
    }
    if (minimized) {
        if (usable) NoteMinimized(e.source, app->name, SUCCEEDED(drawn) ? Kept::Shown : Kept::NotShown, drawn, size);
        else NoteMinimized(e.source, app->name, FAILED(hr) ? Kept::NoAnswer : Kept::NoImage, hr, size);
    }
    if (!fallback.empty()) Unthumb(e.thumbnail);
}

// The last card of the page shown, plus one.
int WindowPreview::PageEnd() const { return std::min(first + capacity, (int)entries.size()); }

int WindowPreview::Hit(POINT p) const {
    if (first > 0 && PtInRect(&previous, p)) return -2;
    if (first + capacity < (int)entries.size() && PtInRect(&next, p)) return -3;
    for (int i = first; i < PageEnd(); ++i)
        if (PtInRect(&entries[i].card, p)) return i;
    return -1;
}

// The close button: a square at the right end of a card's title row, as on the taskbar's previews.
RECT WindowPreview::CloseRect(const Entry& e) const { return CloseRect(e, Measure()); }

RECT WindowPreview::CloseRect(const Entry& e, const Metrics& m) const {
    const int header = m.header, size = Px(24, scale), margin = (header - size) / 2;
    return {e.card.right - margin - size, e.card.top + margin, e.card.right - margin, e.card.top + margin + size};
}

int WindowPreview::HitClose(POINT p) const {
    const Metrics m = Measure();
    for (int i = first; i < PageEnd(); ++i) {
        RECT r = CloseRect(entries[i], m);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

// The card's window is the one (handle and process) a press or a middle press went down on: a handle reused by
// another window since does not count.
bool WindowPreview::SameWindow(int index, HWND source, DWORD pid) const {
    return index >= 0 && source != nullptr && entries[index].source == source && entries[index].pid == pid;
}

// As the window's own close button (AppModel::AskToClose): the program may ask to save first, or go
// to its tray icon. The card stays, dimmed, until the window has gone; the popup stays with the rest.
void WindowPreview::AskClose(int index) {
    if (index < 0 || index >= (int)entries.size()) return;
    Entry& e = entries[index];
    if (e.closing || !Valid(e)) return;
    e.closing = true; e.closingSince = -1;
    closeSent = true;
    const HWND source = e.source;
    const std::wstring name = app->name;
    g_apps.AskToClose(source, name);   // may wait on the administrator helper, or offer to install it
    if (!app || !hwnd) return;
    Arrange();
    if (app) InvalidateRect(hwnd, nullptr, FALSE);
}

void WindowPreview::ChangePage(int delta) {
    int target = std::clamp(first + delta * capacity, 0, (((int)entries.size() - 1) / capacity) * capacity);
    if (target == first) return;
    first = target; pressed = pressedClose = middle = nullptr; pressedNav = 0; hot = hotClose = -1;
    ReleaseThumbnails(); Arrange(); InvalidateRect(hwnd, nullptr, FALSE);
}

void WindowPreview::DrawClose(HDC dc, RECT r, bool over, bool down) const {
    if (over) Fill(dc, r, down ? RGB(148, 31, 21) : RGB(196, 43, 28));   // the red of Windows' close buttons
    const COLORREF ink = over ? RGB(255, 255, 255) : RGB(214, 219, 228);
    wchar_t face[LF_FACESIZE]{};
    HGDIOBJ oldFont = iconFont ? SelectObject(dc, iconFont) : nullptr;
    if (oldFont && GetTextFaceW(dc, LF_FACESIZE, face) && wcscmp(face, L"Segoe MDL2 Assets") == 0) {
        SetTextColor(dc, ink);
        DrawTextW(dc, L"\xE8BB", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);   // ChromeClose
    } else {   // without that font: two strokes
        HPEN pen = CreatePen(PS_SOLID, Px(1.2f, scale), ink);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, k = Px(4.5f, scale);
        MoveToEx(dc, cx - k, cy - k, nullptr); LineTo(dc, cx + k + 1, cy + k + 1);
        MoveToEx(dc, cx + k, cy - k, nullptr); LineTo(dc, cx - k - 1, cy + k + 1);
        SelectObject(dc, oldPen); DeleteObject(pen);
    }
    if (oldFont) SelectObject(dc, oldFont);
}

void WindowPreview::Paint() {
    PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
    RECT client{}; GetClientRect(hwnd, &client);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, client.right), std::max(1L, client.bottom));
    auto oldBitmap = SelectObject(mem, bitmap);
    auto oldFont = SelectObject(mem, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(240, 242, 247));
    Fill(mem, client, RGB(27, 30, 37));
    if (app) {
        const Metrics m = Measure();
        const int pad = m.pad, header = m.header;
        RECT title{pad * 2, pad, previous.left, pad + header};
        std::wstring label = app->name;
        if ((int)entries.size() > capacity) label += L"  " + std::to_wstring(first + 1) + L"–" +
            std::to_wstring(PageEnd()) + L" / " + std::to_wstring(entries.size());
        DrawTextW(mem, label.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        auto arrow = [&](RECT r, const wchar_t* text, bool enabled, int id) {
            if (hot == id) Fill(mem, r, RGB(65, 72, 87));
            SetTextColor(mem, enabled ? RGB(240, 242, 247) : RGB(83, 87, 96));
            DrawTextW(mem, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        if ((int)entries.size() > capacity) {
            arrow(previous, L"‹", first > 0, -2);
            arrow(next, L"›", first + capacity < (int)entries.size(), -3);
        }
        for (int i = first; i < PageEnd(); ++i) {
            auto& e = entries[i];
            Fill(mem, e.card, hot == i ? RGB(64, 77, 97) : RGB(40, 44, 54));
            SetTextColor(mem, e.closing ? RGB(128, 134, 145) : RGB(240, 242, 247));
            const RECT closeRect = CloseRect(e, m);
            RECT titleRect{e.card.left + pad, e.card.top, closeRect.left - Px(2, scale), e.card.top + header};
            DrawTextW(mem, e.title.c_str(), -1, &titleRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            // the close button while the pointer is on the card (or the button is held), not while it closes
            if (!e.closing && (hot == i || hotClose == i || pressedClose == e.source))
                DrawClose(mem, closeRect, hotClose == i, pressedClose == e.source && hotClose == i);
            Fill(mem, e.image, RGB(20, 23, 29));
            if (!e.fallback.empty()) {
                SetTextColor(mem, RGB(183, 192, 205)); RECT textRect = e.image;
                DrawTextW(mem, e.fallback.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
    }
    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont); SelectObject(mem, oldBitmap); DeleteObject(bitmap); DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WindowPreview::Proc(HWND h, UINT message, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<WindowPreview*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<WindowPreview*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(h, message, wp, lp);
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: self->Paint(); return 0;
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int x = self->HitClose(pt), hit = x >= 0 ? -1 : self->Hit(pt);
        self->pressed = hit >= 0 ? self->entries[hit].source : nullptr;
        self->pressedClose = x >= 0 ? self->entries[x].source : nullptr;
        self->pressedPid = hit >= 0 ? self->entries[hit].pid : x >= 0 ? self->entries[x].pid : 0;
        self->pressedNav = hit < -1 ? hit : 0;
        if (x >= 0) InvalidateRect(h, nullptr, FALSE);   // the button, held
        SetCapture(h); return 0;
    }
    case WM_LBUTTONUP: {
        HWND pressed = self->pressed, pressedClose = self->pressedClose; DWORD pid = self->pressedPid; int nav = self->pressedNav;
        self->pressed = self->pressedClose = nullptr; self->pressedNav = 0;
        if (GetCapture() == h) ReleaseCapture();
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        // Let go on a close button: its window closes if the button was pressed too. Never a click on
        // the card: the pointer was on the button, not on the window's picture.
        const int x = self->HitClose(pt);
        if (x >= 0) {
            if (self->SameWindow(x, pressedClose, pid)) self->AskClose(x);
            else InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (pressedClose) InvalidateRect(h, nullptr, FALSE);   // the button, let go elsewhere
        int hit = self->Hit(pt);
        if (nav && nav == hit) self->ChangePage(nav == -2 ? -1 : 1);
        else if (self->SameWindow(hit, pressed, pid) && self->Valid(self->entries[hit])) {
            auto app = self->app; self->Dismiss();
            g_apps.ActivateWindow(pressed); app->attention = false;
        }
        return 0;
    }
    case WM_MBUTTONDOWN: {   // a middle click on a card closes its window, as on the taskbar's previews
        int hit = self->Hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        self->middle = hit >= 0 ? self->entries[hit].source : nullptr;
        self->middlePid = hit >= 0 ? self->entries[hit].pid : 0;
        SetCapture(h); return 0;
    }
    case WM_MBUTTONUP: {
        HWND middle = self->middle; DWORD pid = self->middlePid;
        self->middle = nullptr; self->middlePid = 0;
        if (GetCapture() == h) ReleaseCapture();
        int hit = self->Hit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (self->SameWindow(hit, middle, pid)) self->AskClose(hit);
        return 0;
    }
    case WM_CAPTURECHANGED: self->pressed = self->pressedClose = self->middle = nullptr; self->pressedNav = 0; return 0;
    case WM_MOUSEWHEEL: {   // a page a notch: a precision touchpad or a free-spinning wheel sends a notch in smaller steps
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (!self->app || self->entries.empty() || !delta) return 0;
        if ((self->wheel > 0) != (delta > 0)) self->wheel = 0;   // turned the other way: a new turn
        self->wheel += delta;
        // a turn at the first or last page is spent, not saved; a page change that puts the popup away ends it
        while (self->app && self->wheel >= WHEEL_DELTA) { self->wheel -= WHEEL_DELTA; self->ChangePage(-1); }
        while (self->app && self->wheel <= -WHEEL_DELTA) { self->wheel += WHEEL_DELTA; self->ChangePage(1); }
        return 0;
    }
    case WM_CLOSE: self->Dismiss(); return 0;
    case WM_NCDESTROY:
        self->hwnd = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(h, message, wp, lp);
}
