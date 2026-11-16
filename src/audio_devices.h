#pragma once
#include "common.h"

// Audio output devices: the list Windows shows under "Output", and switching the default one.
namespace audio {
    struct Device { std::wstring id, name; bool isDefault = false; };
    std::vector<Device> Outputs();                 // active playback devices
    // The default playback device's name ("" when there is none), for the audio button's tooltip,
    // which is built every frame while the pointer rests on it: read at most once a second, and
    // without listing every device (UI thread).
    std::wstring DefaultOutputName();
    bool SetDefault(const std::wstring& id);       // false: the caller opens the sound settings instead
}
