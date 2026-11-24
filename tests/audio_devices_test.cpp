// The audio button's tooltip (audio::DefaultOutputName, src/audio_devices.cpp) on Windows (or Wine): it
// names the device the button's menu marks as the default, is read at most once a second (the tooltip
// is built every frame while the pointer rests on the button; it used to list every playback device
// and open each one's properties every frame), and a device switch made by the dock is seen at once.
// Touches nothing: it only reads. SetDefault is called once with an id no device has, which fails.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\audio_devices_test.cpp /link ole32.lib oleaut32.lib uuid.lib propsys.lib user32.lib
#include "../src/audio_devices.cpp"
#include <cstdarg>
#include <cstdio>

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    char utf8[2048] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    printf("  log: %s\n", utf8);
}
static double g_clockShift = 0;   // seconds added to the clock: the second that passes, a clock that goes back
double NowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart) + g_clockShift;
}

#include "portable_check.h"
static const bool kFlushEachCheck = (portable_check::FlushEach() = true);
static std::string Utf8(const std::wstring& w) {
    char b[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // as the dock's UI thread

    std::wstring menuDefault;   // what the button's menu ticks
    const auto outputs = audio::Outputs();
    for (auto& d : outputs) if (d.isDefault) menuDefault = d.name;
    printf("  %d playback device(s); default: \"%s\"\n", (int)outputs.size(), Utf8(menuDefault).c_str());

    {
        int defaults = 0; bool named = true, unique = true;
        for (size_t i = 0; i < outputs.size(); ++i) {
            defaults += outputs[i].isDefault;
            if (outputs[i].name.empty() || outputs[i].id.empty()) named = false;
            for (size_t j = i + 1; j < outputs.size(); ++j) if (outputs[i].id == outputs[j].id) unique = false;
        }
        Check(defaults <= 1 && named && unique, "the menu's list: at most one default, every device with an id and a name, no id twice");
    }

    const std::wstring tip = audio::DefaultOutputName();
    Check(tip == menuDefault, "the tooltip names the device the menu marks as default (\"\" when there is none)");

    // a second of frames at 144 Hz: one read (the first look above), the rest are the kept name
    double t0 = NowSeconds();
    bool same = true;
    for (int i = 0; i < 144; ++i) same &= audio::DefaultOutputName() == tip;
    double hits = NowSeconds() - t0;
    Check(same, "looks within the second give the same name");

    // what one look cost before (every device, every property store) and what a read costs now
    t0 = NowSeconds();
    for (int i = 0; i < 20; ++i) audio::Outputs();
    double list = (NowSeconds() - t0) / 20;
    t0 = NowSeconds();
    for (int i = 0; i < 20; ++i) audio::ReadDefaultName();
    double one = (NowSeconds() - t0) / 20;
    printf("  per frame before: %.3f ms (every device); a read now: %.3f ms, once a second; a kept look: %.4f ms\n",
           list * 1000, one * 1000, hits * 1000 / 144);
    Check(hits < 0.05, "144 kept looks cost next to nothing (no call into the audio service)");

    // a switch made from the dock's menu: the kept name is dropped, the next look reads
    audio::g_defaultName.Invalidate();
    audio::g_defaultName.Get(NowSeconds(), 1.0, [] { return std::wstring(L"(kept)"); });
    Check(audio::DefaultOutputName() == L"(kept)", "within the second the kept name is shown (not read again)");
    Check(!audio::SetDefault(L"{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}"), "SetDefault with an id no device has fails (nothing changes)");
    Check(audio::DefaultOutputName() == menuDefault, "...and the next look reads the device again at once");

    // the second passes: the next look reads again; a clock that goes back reads too
    audio::g_defaultName.Get(NowSeconds(), 0.0, [] { return std::wstring(L"(kept)"); });
    Check(audio::DefaultOutputName() == L"(kept)", "a name just read is kept for the second");
    g_clockShift += 1.0;
    Check(audio::DefaultOutputName() == menuDefault, "...a second later the device is read again (a switch made in Windows shows within a second)");
    audio::g_defaultName.Get(NowSeconds(), 0.0, [] { return std::wstring(L"(kept)"); });
    g_clockShift -= 100.0;
    Check(audio::DefaultOutputName() == menuDefault, "...and a clock that went back does not keep the old name until it catches up");
    g_clockShift = 0;

    CoUninitialize();
    return Summary("audio_devices_test");
}
