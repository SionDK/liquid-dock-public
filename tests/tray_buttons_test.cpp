// Which stock Windows 11 tray button is the hidden-icons chevron (src/tray_buttons.h), from the class
// names and automation ids UI Automation reports, in tree order. No Windows headers.
//   cl /std:c++20 /EHsc /utf-8 tests\tray_buttons_test.cpp
//   g++ -std=c++20 tests/tray_buttons_test.cpp -o tray_buttons_test
#include "../src/tray_buttons.h"
#include "portable_check.h"

using namespace traybuttons;

namespace {
Facts Normal(const wchar_t* id) { return { kNormal, id }; }
}

int main() {
    // Windows 11 26H2: app icons shown on the taskbar are NormalButtons too
    Check(ChevronIndex({ Normal(L"NotifyItemIcon"), Normal(L"NotifyItemIcon"), Normal(L"SystemTrayIcon") }) == 2,
          "26H2: the chevron after the app icons is found, not the first app icon");
    Check(ChevronIndex({ Normal(L"SystemTrayIcon"), Normal(L"NotifyItemIcon") }) == 0, "26H2: chevron first");
    Check(ChevronIndex({ Normal(L"NotifyItemIcon"), Normal(L"NotifyItemIcon") }) == -1,
          "26H2 with the hidden icon menu off: no chevron (never an app icon)");
    Check(ChevronIndex({ Normal(L"NotifyItemIcon"), Normal(L"") }) == -1, "26H2: a button without an id is not taken for the chevron");
    Check(ChevronIndex({ Normal(L"NotifyItemIcon"), Normal(L"SomethingNew") }) == 1, "a renamed chevron id still beats app icons");

    // older Windows 11 builds: app icons have another class, the first NormalButton is the chevron
    Facts appIcon = { L"SystemTray.NotifyIconButton", L"" };
    Check(ChevronIndex({ appIcon, Normal(L""), Normal(L"") }) == 1, "older builds: the first NormalButton");
    Check(ChevronIndex({ appIcon, appIcon }) == -1, "older builds without a chevron");
    Check(ChevronIndex({}) == -1, "nothing found (taskbar content not there yet)");

    // the chevron's own id wins over whatever comes before it
    Check(ChevronIndex({ Normal(L"NotifyItemIcon"), Normal(L"SomethingNew"), Normal(L"SystemTrayIcon") }) == 2,
          "26H2: the chevron's id beats an unknown button in front of it");
    Check(ChevronIndex({ Normal(L"SomethingNew"), Normal(L"SystemTrayIcon") }) == 1,
          "no app icons: the chevron's id beats the first NormalButton");
    Check(ChevronIndex({ Normal(L"NotifyItemIcon"), Normal(L"SystemTrayIcon"), Normal(L"SystemTrayIcon") }) == 1,
          "two buttons with the chevron's id: the first");
    // an app icon of another class says nothing about the NormalButtons
    Check(ChevronIndex({ { L"SystemTray.NotifyIconButton", L"NotifyItemIcon" }, Normal(L"") }) == 1,
          "an app-icon id on another class: the NormalButton without an id is still the chevron");

    // other classes never count, whatever their id
    Check(ChevronIndex({ { L"SystemTray.AccentButton", L"SystemTrayIcon" }, { L"SystemTray.OmniButton", L"SystemTrayIcon" },
                         Normal(L"SystemTrayIcon") }) == 2, "network and clock buttons share the id but not the class");

    // every list of up to four buttons where no NormalButton has an app icon's or the chevron's id (older
    // builds): the first NormalButton, -1 without one
    {
        const Facts kinds[] = { Normal(L""), Normal(L"SomethingNew"), appIcon, { L"SystemTray.AccentButton", L"SystemTrayIcon" } };
        int lists = 0, wrong = 0;
        for (int len = 0; len <= 4; ++len)
            for (int code = 0, end = 1 << (2 * len); code < end; ++code) {
                std::vector<Facts> list;
                int first = -1;
                for (int i = 0; i < len; ++i) {
                    list.push_back(kinds[(code >> (2 * i)) & 3]);
                    if (first < 0 && list.back().className == kNormal) first = i;
                }
                ++lists;
                if (ChevronIndex(list) != first) ++wrong;
            }
        Check(lists == 341 && wrong == 0, "older builds, all 341 lists of up to four buttons: the first NormalButton");
    }
    return Summary("tray_buttons_test");
}
