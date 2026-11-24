#pragma once
// FluentFlyout's two integration tests (fluent_flyout_test, fluent_lifecycle_test):
// the helpers they share. Include it after FluentFlyout's parts (src\fluent_*.cpp: SetSessions sets
// fluent_media.cpp's session state). Each test keeps its own name fakes (its own prefix for the class names and the
// title, before the parts), Log, Check, NowSeconds, WindowThread, WriteSettings and MakeFlyout - they differ.
#include <functional>
#include <mutex>
#include <string>
#include <vector>

static void Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

// cond() every 20 ms (the test's own messages pumped first) until it holds or ms have passed
static bool WaitFor(const std::function<bool()>& cond, int ms) {
    for (int i = 0; i < ms / 20; ++i) { Pump(); if (cond()) return true; Sleep(20); }
    return cond();
}

// the media sessions, set by hand (the tests build with LD_FLUENT_NO_MEDIA: the real ones are never read)
static void SetSessions(std::vector<std::wstring> ids) {
    std::lock_guard<std::mutex> lk(fluentflyout::g_mediaMu);
    fluentflyout::g_sessions = std::move(ids);
    fluentflyout::g_mediaKnown = true;
    ++fluentflyout::g_mediaVersion;
}

static RECT Rect(HWND h) { RECT r{}; GetWindowRect(h, &r); return r; }
