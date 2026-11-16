#pragma once
#include "common.h"
#include "battery.h"

// The small square buttons of the dock's tray. Network and volume sit before the clock, settings
// is always last; everything in between is the user's choice and order (Settings::trayButtons).
enum class ButtonId { Network, Volume, Desktop, Theme, Oled, Awake, Screenshot, Quiet, Trash, Downloads, Audio, Sff, Settings, Count };
constexpr int kButtonCount = (int)ButtonId::Count;

struct ButtonInfo {
    ButtonId id;
    const char* key;            // in settings.ini
    const wchar_t* name;        // in the settings window
    const wchar_t* description;
    bool fixed;                 // always shown, cannot be moved (network, volume, settings)
    float iconFactor = 0.55f, minWidth = 24;   // cell width = max(icon * factor, minWidth * scale)
};
const ButtonInfo& Info(ButtonId id);
const ButtonInfo* FindButton(const std::string& key);

// "desktop,theme" <-> ordered ids. Unknown keys, duplicates and fixed buttons are dropped.
std::vector<ButtonId> ParseButtons(const std::string& list);
std::string FormatButtons(const std::vector<ButtonId>& ids);
// g_settings.trayButtons, parsed (cached until the setting changes)
const std::vector<ButtonId>& ChosenButtons();
bool HasButton(ButtonId id);
void SetButton(ButtonId id, bool on);   // added at the end / removed

// Places ids from x along the dock: cell width from the catalogue; with stack, two neighbours share a
// cell (half 0 / 1), an odd one keeps a whole cell. Shared by the dock and the settings preview.
struct PlannedCell { ButtonId id; float u0, u1; int half; };
float PlanButtons(const std::vector<ButtonId>& ids, float icon, float scale, bool stack, float x, std::vector<PlannedCell>* out);

// Live state some buttons show (and act on).
namespace features {
    bool Awake();                 // display and sleep kept on
    void SetAwake(bool on);
    bool Quiet(double now);       // notification bounces suppressed
    void SetQuiet(double until);  // 0: off, a huge value: until switched off
    bool TrashFull();             // polled every few seconds
    void Tick(double now);

    // newest first; skips folders, hidden files and unfinished downloads (.crdownload, .part, .tmp)
    struct RecentFile { std::wstring path, name, ago; };
    std::vector<RecentFile> RecentDownloads(size_t max);
    std::vector<RecentFile> RecentFilesIn(const std::wstring& dir, size_t max);   // the same, for any folder
    std::wstring DownloadsFolder();
}

// Icons, shared by the dock and the settings preview so both always look the same.
struct IconState {
    int net = 1;                  // 0 none, 1 ethernet, 2 Wi-Fi
    float volume = 0.5f; bool muted = false;
    bool oled = false, light = false, awake = false, quiet = false, trashFull = false;
};
IconState CurrentIconState(double now);
// glyphFont: the Segoe Fluent / MDL2 icon font sized for scale, centred both ways; scale: dock scale
// (1 = a 1440 px short side); zoom: 0.8 when two buttons share a cell; shadow: alpha of the soft
// shadow under glyph icons (0 for none).
void DrawButtonIcon(ButtonId id, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, IDWriteTextFormat* glyphFont,
                    D2D1_RECT_F cell, float scale, float zoom, D2D1_COLOR_F ink, float shadow, const IconState& st);

// The battery cell's icon (battery.h), larger than the small buttons' icons: body and terminal, the
// charge as a bar, a bolt while charging. box: the icon (about 2:1); ink: the outline; shadow: alpha of
// the soft shadow under it (0 for none). Shared by the dock and the settings preview.
void DrawBatteryIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, D2D1_RECT_F box, float scale,
                     D2D1_COLOR_F ink, float shadow, const battery::State& st);
// The charge's colour: green on mains power, amber with battery saver, orange when low, red when
// critical, otherwise the ink. Also the percentage's colour when low.
D2D1_COLOR_F BatteryColor(const battery::State& st, D2D1_COLOR_F ink);
// The icon's size for an app icon size: width, height (about 2:1). At least half again as wide as
// the small buttons' glyphs (17 px at 100 %), however small the app icons get.
inline float BatteryIconWidth(float icon, float scale) { return std::round(std::max(icon * 0.66f, 26.0f * scale)); }
inline float BatteryIconHeight(float icon, float scale) { return std::round(BatteryIconWidth(icon, scale) * 0.5f); }
