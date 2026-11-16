#pragma once
// The decisions of the FluentFlyout module (fluent_flyout.cpp, fluent_media.cpp), kept free of Windows headers so they
// can be tested with any C++20 compiler (tests/fluent_logic_test.cpp; also run by the local-LLM task
// pack in tools/llm-tasks).
#include <algorithm>
#include <vector>

namespace fluentlogic {

// One monitor as the system lists it (EnumDisplayMonitors order).
struct Monitor {
    bool primary = false;
    long left = 0;
};

// FluentFlyout numbers monitors ("1", "2" ... in its settings) primary first, then from left to right,
// keeping the system's order between equals (MonitorUtil.GetMonitors: OrderByDescending(isPrimary)
// .ThenBy(Left), a stable sort). The result lists positions in `monitors`.
inline std::vector<int> FluentOrder(const std::vector<Monitor>& monitors) {
    std::vector<int> order(monitors.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (monitors[a].primary != monitors[b].primary) return monitors[a].primary;
        return monitors[a].left < monitors[b].left;
    });
    return order;
}

// The monitor one of FluentFlyout's monitor settings stands for (FluentFlyout clamps the number to its
// list): a position in `monitors`, or -1 when there is none.
inline int MonitorForSetting(const std::vector<Monitor>& monitors, int setting) {
    if (monitors.empty()) return -1;
    const std::vector<int> order = FluentOrder(monitors);
    return order[std::clamp(setting, 0, (int)order.size() - 1)];
}

// FluentFlyout's number ("1", "2" ...) of a monitor: 1-based, 0 when it is not in the list.
inline int SettingNumber(const std::vector<Monitor>& monitors, int monitor) {
    const std::vector<int> order = FluentOrder(monitors);
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == monitor) return (int)i + 1;
    return 0;
}

// The primary monitor's position, the first monitor without one, -1 without monitors.
inline int PrimaryOf(const std::vector<Monitor>& monitors) {
    for (size_t i = 0; i < monitors.size(); ++i)
        if (monitors[i].primary) return (int)i;
    return monitors.empty() ? -1 : 0;
}

// ---------------------------------------------------------------- which dock holds the widget
// FluentFlyout picks "the taskbar" for its widget every 1.5 s: the first Shell_TrayWnd window (the
// dock's host window while it runs) when that one is on the monitor FluentFlyout is set to; otherwise
// a secondary Explorer taskbar on that monitor; and when it finds none, the first Shell_TrayWnd anyway.
// So its widget inside a secondary Explorer taskbar is a sure answer: FluentFlyout is set to that
// taskbar's monitor. Inside Explorer's main taskbar (the first Shell_TrayWnd while the dock's host
// window is not up) only while Explorer has a taskbar on every display (ShowsMonitor). Inside the
// dock's window, or Explorer's main taskbar without one on every display, it may sit there only by
// that last fallback (FluentFlyout set to the main monitor while the dock's window is elsewhere, or set
// to a monitor without a taskbar), so there its settings file decides - unless FluentFlyout has shown
// otherwise since the file last changed. Without either, the main monitor: from there FluentFlyout's
// choice always shows (a widget for another monitor moves into Explorer's taskbar there).
inline bool ShowsMonitor(bool secondaryTaskbar, int secondaryTaskbars, int monitors) {
    return secondaryTaskbar || secondaryTaskbars >= monitors - 1;
}

struct Choice {
    int fileMonitor = -1;      // from FluentFlyout's settings file (-1: not read)
    double fileAt = -1;        // when that value was first read
    int shownMonitor = -1;     // where FluentFlyout last put its widget into one of Explorer's taskbars
    double shownAt = -1;       // when that was last seen
};

enum class Reason { File, Shown, Default };

inline int WantedMonitor(const Choice& c, int primary, Reason* why = nullptr) {
    Reason r = Reason::Default;
    int m = primary;
    if (c.shownMonitor >= 0 && c.shownAt >= c.fileAt) { r = Reason::Shown; m = c.shownMonitor; }
    else if (c.fileMonitor >= 0) { r = Reason::File; m = c.fileMonitor; }
    if (why) *why = r;
    return m;
}

// ---------------------------------------------------------------- "something is playing"
// FluentFlyout's "hide completely when nothing is playing" follows its media sessions. Sessions are
// replaced now and then (a browser ends one and starts the next), so the list is empty for a moment:
// playing counts at once, "nothing playing" only once it has lasted `hold` seconds. The slot then
// closes once instead of flickering.
struct Steady {
    bool value = true;         // what counts now (unknown counts as playing)
    double offSince = -1;      // the raw state has said "nothing playing" since then

    // Feed the raw state; true when `value` changed.
    bool Update(bool raw, double now, double hold = 1.5) {
        if (raw) {
            offSince = -1;
            if (value) return false;
            value = true;
            return true;
        }
        if (!value) return false;
        if (offSince < 0) offSince = now;
        if (now - offSince < hold) return false;
        value = false;
        return true;
    }
    bool Waiting() const { return value && offSince >= 0; }   // "nothing playing" is being held back
};

// ---------------------------------------------------------------- the widget window coming back
// FluentFlyout closes its widget window and makes a new one whenever the displays or the work area
// change (a dock reserving its strip does that). While FluentFlyout runs, the dock keeps its place for
// the widget for a short while instead of closing the slot and opening it again a moment later.
constexpr double kRehomeSeconds = 3.0;

inline bool StillWaiting(double lostAt, double now) { return lostAt >= 0 && now - lostAt < kRehomeSeconds; }

}   // namespace fluentlogic
