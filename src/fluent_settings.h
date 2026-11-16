#pragma once
// FluentFlyout's settings file (%APPDATA%\FluentFlyout\settings.xml, written by .NET's XmlSerializer),
// read without Windows headers so it can be tested with any C++20 compiler
// (tests/fluent_settings_test.cpp). Only the values the dock uses; missing values take FluentFlyout's
// defaults. Not a general XML parser: XmlSerializer writes one element per line, no attributes on
// these values, the five standard entities.
#include <string>
#include <utility>
#include <vector>

namespace fluentsettings {

struct Settings {
    bool loaded = false;                        // the text is FluentFlyout's settings (<UserSettings ...>)
    bool widgetEnabled = false, hideCompletely = false, visualizer = false;
    int widgetMonitor = 0, manualPadding = 0;   // TaskbarWidgetSelectedMonitor: 0-based, FluentFlyout's order
    int animationSpeed = 2;                     // flyout animation: 0 off, 1 0.5x, 2 1x ... 5 3x
    bool filtering = false; int filterMode = 0; // 0: block list, 1: allow list
    std::vector<std::wstring> allowed, blocked;
};

inline std::wstring Trimmed(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

// The five XML entities; &amp; last, so "&amp;lt;" stays the text "&lt;".
inline std::wstring XmlText(std::wstring s) {
    static const std::pair<const wchar_t*, const wchar_t*> kEntities[] = {
        { L"&lt;", L"<" }, { L"&gt;", L">" }, { L"&quot;", L"\"" }, { L"&apos;", L"'" }, { L"&amp;", L"&" } };
    for (auto& [entity, ch] : kEntities) {
        const std::wstring e = entity, c = ch;
        for (size_t p = 0; (p = s.find(e, p)) != std::wstring::npos; p += c.size()) s.replace(p, e.size(), c);
    }
    return s;
}

// The text between <name> and </name>, the first such element (an empty <name /> counts as missing).
inline bool Element(const std::wstring& xml, const std::wstring& name, std::wstring& out) {
    std::wstring open = L"<" + name + L">", close = L"</" + name + L">";
    size_t a = xml.find(open);
    if (a == std::wstring::npos) return false;
    a += open.size();
    size_t b = xml.find(close, a);
    if (b == std::wstring::npos) return false;
    out = xml.substr(a, b - a);
    return true;
}

inline bool ReadBool(const std::wstring& xml, const wchar_t* name, bool fallback) {
    std::wstring v;
    return Element(xml, name, v) ? Trimmed(v) == L"true" : fallback;
}

inline int ReadInt(const std::wstring& xml, const wchar_t* name, int fallback) {
    std::wstring v;
    if (!Element(xml, name, v)) return fallback;
    try { return std::stoi(Trimmed(v)); } catch (...) { return fallback; }
}

// <name><string>a</string><string>b</string></name>, entities decoded.
inline std::vector<std::wstring> ReadList(const std::wstring& xml, const wchar_t* name) {
    std::vector<std::wstring> out;
    std::wstring section;
    if (!Element(xml, name, section)) return out;
    for (size_t p = 0;;) {
        size_t a = section.find(L"<string>", p);
        if (a == std::wstring::npos) break;
        a += 8;
        size_t b = section.find(L"</string>", a);
        if (b == std::wstring::npos) break;
        out.push_back(XmlText(section.substr(a, b - a)));
        p = b + 9;
    }
    return out;
}

inline Settings Parse(const std::wstring& xml) {
    Settings s;
    if (xml.find(L"<UserSettings") == std::wstring::npos) return s;
    s.loaded = true;
    s.widgetEnabled = ReadBool(xml, L"TaskbarWidgetEnabled", false);
    s.hideCompletely = ReadBool(xml, L"TaskbarWidgetHideCompletely", false);
    s.visualizer = ReadBool(xml, L"TaskbarVisualizerEnabled", false);
    s.widgetMonitor = ReadInt(xml, L"TaskbarWidgetSelectedMonitor", 0);
    s.manualPadding = ReadInt(xml, L"TaskbarWidgetManualPadding", 0);
    s.animationSpeed = ReadInt(xml, L"FlyoutAnimationSpeed", 2);
    s.filtering = ReadBool(xml, L"AppFilteringEnabled", false);
    s.filterMode = ReadInt(xml, L"AppFilteringMode", 0);
    s.allowed = ReadList(xml, L"AllowedApps");
    s.blocked = ReadList(xml, L"BlockedApps");
    return s;
}

}   // namespace fluentsettings
