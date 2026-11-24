#pragma once
#include "apps.h"
#include <dwmapi.h>

// The preview owns its popup and DWM relationships. It never touches Explorer's
// taskbar, restores a source window for capture, or takes screenshots of it. A
// minimized window shows the image DWM kept of it, as on the taskbar. A card's close
// button (shown while the pointer is on the card) or a middle click on the card asks
// that window to close, through the dock's AppModel; the popup stays with the rest.
class WindowPreview {
public:
    ~WindowPreview();
    WindowPreview() = default;
    WindowPreview(const WindowPreview&) = delete;
    WindowPreview& operator=(const WindowPreview&) = delete;
    void Update(HWND owner, const std::shared_ptr<AppItem>& hovered, RECT anchor,
                RECT monitor, Edge edge, float scale, POINT cursor, double now, bool allowed);
    void Dismiss();
    bool Open() const;
    bool Contains(POINT cursor) const;

private:
    struct Entry {
        HWND source = nullptr;
        DWORD pid = 0;
        std::wstring title, fallback;
        HTHUMBNAIL thumbnail = nullptr;
        RECT card{}, image{};
        bool noImage = false;   // minimized, and DWM kept no image of it: the placeholder until it is restored
        bool closing = false;   // asked to close: dimmed, its close button rests until kCloseAgain has passed
        double closingSince = -1;   // Update's clock; -1 until the next Update stamps it
        RECT sentDestination{};     // what DWM has for this thumbnail (empty: nothing sent since it was registered)
        BYTE sentOpacity = 0;
    };
    // The popup's measures at its scale, each rounded on its own (Px): Arrange, CloseRect and Paint use the same.
    struct Metrics { int pad, gap, header, nav; };
    // Where Arrange puts the popup (screen coordinates) and how the cards of its page lie in it.
    struct Placement { bool vertical; int cw, ch, count, x, y, w, h; };
    bool Create(HWND owner);
    void Hide();
    void ReleaseThumbnails();
    void Refresh();
    void Arrange(bool follow = false);
    Metrics Measure() const;
    bool PlacePopup(const Metrics& m, Placement& at);
    void PlaceCards(const Metrics& m, const Placement& at, bool& moved);
    void ShowThumbnail(Entry& e, const RECT& image, bool minimized, const wchar_t* none, std::wstring& fallback);
    void Paint();
    void DrawClose(HDC dc, RECT r, bool over, bool down) const;
    void ChangePage(int delta);
    int PageEnd() const;
    int Hit(POINT client) const;
    int HitClose(POINT client) const;
    RECT CloseRect(const Entry& entry) const;
    RECT CloseRect(const Entry& entry, const Metrics& m) const;
    bool SameWindow(int index, HWND source, DWORD pid) const;
    void AskClose(int index);
    bool Valid(const Entry& entry) const;
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd = nullptr;
    HFONT font = nullptr, iconFont = nullptr;
    std::shared_ptr<AppItem> candidate, app;
    std::vector<Entry> entries;
    RECT anchor{}, monitor{}, bounds{}, bridge{}, previous{}, next{};
    Edge edge = Edge::Bottom;
    float scale = 1;
    int first = 0, capacity = 1, hot = -1, hotClose = -1;
    HWND pressed = nullptr, pressedClose = nullptr, middle = nullptr, foreground = nullptr;
    DWORD pressedPid = 0, middlePid = 0;
    int pressedNav = 0;
    double since = 0, outsideSince = -1, nextRefresh = 0;
    double closeGrace = -1;    // until then a new front window is taken for the one a close brought (Update's clock)
    bool closeSent = false;    // a close went out since the last Update: it stamps closeGrace
    bool suppressed = false;
    int wheel = 0;             // wheel turn short of a whole notch (WHEEL_DELTA): not a page yet
    static inline WindowPreview* active = nullptr;
};
