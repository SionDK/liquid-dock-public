#pragma once
// What oled.cpp (the mode: black wallpaper, hidden icons, the record) and oled_covers.cpp (the black covers over
// live wallpapers) share. Not for other files.
#include "oled.h"
#include <shobjidl.h>   // IDesktopWallpaper
#include <map>

namespace oled {

struct State {
    bool active = false;
    bool iconsHidden = false;        // we hid them (they were visible before)
    bool allMonitors = true;
    COLORREF background = 0;
    std::wstring slideshow;          // slideshow folder that was running, if any
    std::map<std::wstring, std::wstring> wallpapers;   // monitor device path -> original wallpaper
};

// oled.cpp
State Load();
ComPtr<IDesktopWallpaper> Wallpaper(bool retry);   // its default (false): on the definition
// oled_covers.cpp
extern bool g_coversDirty;
void DestroyCovers();

}   // namespace oled
