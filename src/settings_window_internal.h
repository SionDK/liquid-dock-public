#pragma once
// What the parts of the settings window share (settings_window.cpp: the window; settings_window_pages.cpp: the
// options on each tab; settings_window_customizer.cpp: the buttons tab's preview and drag and drop).
#include "common.h"
#include "buttons.h"
#include <map>

namespace settingswindow {

using D2D1::ColorF;

// ---------------------------------------------------------------- the options

// How a change reaches the docks.
enum class Apply {
    None,        // read when needed (delays, OLED scope)
    Live,        // laid out again in place while the control moves (falls back to a rebuild)
    OnRelease,   // the dock windows change size: rebuilt once, when the slider is let go
    Rebuild,     // discrete choice that changes the windows
};

struct Control {
    enum Type { Segmented, Slider, Toggle, Button, Customizer, Group } type;   // Customizer: the button preview and list
    // Group: a header that folds the `count` controls after it; they are not drawn (nor take a click or the
    // wheel) until it is opened, so a slider is never changed by accident. Folded each time the window opens.
    const wchar_t* label;
    const wchar_t* hint = nullptr;                      // one line under the label
    Apply apply = Apply::None;
    // segmented
    std::vector<const wchar_t*> options;
    std::function<int()> index;
    std::function<void(int)> choose;
    // slider
    float lo = 0, hi = 1, step = 1;
    std::function<float()> value;
    std::function<void(float)> set;
    std::function<std::wstring(float)> text;
    // toggle
    std::function<bool()> on;
    std::function<void(bool)> toggle;
    // button
    std::function<void()> press;
    // the value "기본값으로" restores (slider value, segment index, toggle 0/1), shown on the control;
    // filled in by BuildPages. NAN: none (toggles that act now rather than store a setting)
    float def = NAN;
    bool hasDefault = true;
    // group
    int count = 0;
    mutable bool open = false;
    // drawn (with its hot areas and its room on the page) only while this says so; empty: always. An option that
    // means something only under another one's choice (the 센서 page's "CPU 전력" / "GPU 전력" under "전력 소비량")
    std::function<bool()> shown;
    bool Shown() const { return !shown || shown(); }
};

struct Page { const wchar_t* title; const wchar_t* glyph; std::vector<Control> controls; std::function<void()> reset; };

struct Hot { D2D1_RECT_F r; int control; int sub; };

constexpr int kHotDock = -1000;      // - index in the chosen list
constexpr int kHotPalette = -2000;   // - ButtonId

struct ButtonDrag {
    bool armed = false, active = false, fromDock = false;
    ButtonId id = ButtonId::Count;
    float sx = 0, sy = 0, x = 0, y = 0;
};

// settings_window.cpp
extern HWND g_wnd;
extern HMONITOR g_monitor;
extern std::vector<Page> g_pages;
extern int g_hover;
extern bool g_dark;
extern D2D1_COLOR_F g_accent;
extern ComPtr<ID2D1SolidColorBrush> g_brush;
extern ComPtr<IDWriteTextFormat> g_fLabel, g_fHint, g_fChip, g_fTiny;
extern std::map<int, ComPtr<IDWriteTextFormat>> g_glyphFonts;
extern std::vector<Hot> g_hots;
D2D1_COLOR_F Text(float a = 1);
D2D1_COLOR_F Muted();
D2D1_COLOR_F Wash(float a);
ComPtr<IDWriteTextFormat> Font(float size, DWRITE_FONT_WEIGHT w, const wchar_t* family = L"Segoe UI Variable Text");
void Fill(D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0);
void Draw(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c);
// settings_window_pages.cpp
std::wstring Num(float v, int decimals, const wchar_t* unit);
void BuildPages();
// settings_window_customizer.cpp
bool IsNav(int control);
extern ButtonDrag g_bd;
std::vector<ButtonId> ShownList();
void CommitButtons(const std::vector<ButtonId>& list);
float DrawCustomizer(float x0, float y, float x1);
void ButtonMenu(int hot, POINT screen);

}   // namespace settingswindow
