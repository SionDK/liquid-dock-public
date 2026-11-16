#pragma once
// When the background poller of system.cpp (volume, mute, network and battery for the dock's buttons)
// does what, kept free of Windows headers so it can be tested anywhere (tests/sys_poll_test.cpp, run by
// tests/run_portable.sh).
//
// It used to wake twice a second for ever and, every time, open the audio device enumerator, ask for the
// default playback device and activate its volume control - round trips to the audio service - to read
// two numbers; every 3 s it also made a new network list manager and listed every network adapter. On a
// laptop that kept the audio service and the CPU from resting. Now the volume control is opened once per
// default device and reports volume and mute changes itself (IAudioEndpointVolumeCallback); a new
// default device and a change of the network interfaces wake the poller. What is left on a timer is
// cheap: the battery every 5 s (GetSystemPowerStatus, no service), the volume control read now and then
// in case its reports stopped (an audio service restart says nothing to it), and the network once in a
// while in case a change went unreported. A reported network change is followed by a few more reads: the
// link comes up first, the address, the gateway and Windows' connectivity check follow within seconds.
// Should the control not report changes, it is read every 0.5 s as before - without opening anything
// again.
#include <algorithm>

// (std::min) / (std::max) in parentheses: windows.h's min/max macros (a file that includes windows.h without
// NOMINMAX) cannot touch them
namespace syspoll {

constexpr double kVolumePoll = 0.5;     // only while the volume control does not report changes
constexpr double kVolumeCheck = 15.0;   // a control that reports: read all the same now and then (a failed read,
                                        // or a level it did not report, opens it again)
constexpr double kAudioRetry = 5.0;     // no default playback device (yet): look again
constexpr double kNetFollow = 2.0;      // after a reported network change: read every 2 s ...
constexpr double kNetSettle = 12.0;     // ... for 12 s after the last report
constexpr double kNetGap = 1.0;         // a report within 1 s of the last read waits that long (bursts of reports)
constexpr double kNetSafety = 30.0;     // the network although nothing was reported
constexpr double kBattery = 5.0;        // GetSystemPowerStatus (and at once on WM_POWERBROADCAST: main.cpp)
constexpr double kMaxSleep = 5.0;

// What the poller knows between passes.
struct State {
    bool bound = false;          // the default device's volume control is open
    bool reporting = false;      // ... and reports changes itself
    double nextAudio = 0;        // open it then (not open yet, or opening failed)
    double nextVolume = 0;       // read it then (open: every kVolumePoll, or kVolumeCheck while reporting)
    double nextNet = 0;
    double lastNet = -1e9;       // the last network read
    double settleUntil = -1;     // reads every kNetFollow until then (a change was reported)
    double nextBattery = 0;
};

// What happened since the last pass (set by the notifications and by failed reads).
struct Events {
    bool device = false;         // another default playback device, or a playback device came or went
    bool volumeLost = false;     // reading the open control failed, or it missed a change: open it again
    bool network = false;        // an IP interface or address changed
};

// What a pass does.
struct Pass {
    bool bindAudio = false;      // (re)open the default device's volume control, register for its reports, read it
    bool readVolume = false;     // read volume and mute from the open control
    bool readNet = false;
    bool readBattery = false;
};

inline Pass Next(State& s, const Events& e, double now) {
    Pass p;
    if (e.device || e.volumeLost) { s.bound = s.reporting = false; s.nextAudio = now; }
    p.bindAudio = !s.bound && now >= s.nextAudio;
    p.readVolume = s.bound && now >= s.nextVolume;
    if (e.network) {
        s.settleUntil = now + kNetSettle;
        s.nextNet = (std::min)(s.nextNet, (std::max)(now, s.lastNet + kNetGap));
    }
    p.readNet = now >= s.nextNet;
    p.readBattery = now >= s.nextBattery;
    return p;
}

// After the pass: bound / reporting say how opening the control went (when the pass did that). Returns
// how long the poller may sleep before the next timed work (an event wakes it earlier).
inline double Done(State& s, const Pass& p, double now, bool bound, bool reporting) {
    if (p.bindAudio) {
        s.bound = bound;
        s.reporting = bound && reporting;
        if (!bound) s.nextAudio = now + kAudioRetry;
    }
    if (p.readVolume || (p.bindAudio && bound)) s.nextVolume = now + (s.reporting ? kVolumeCheck : kVolumePoll);
    if (p.readNet) { s.lastNet = now; s.nextNet = now + (now < s.settleUntil ? kNetFollow : kNetSafety); }
    if (p.readBattery) s.nextBattery = now + kBattery;
    double next = (std::min)({ s.nextBattery, s.nextNet, s.bound ? s.nextVolume : s.nextAudio });
    return std::clamp(next - now, 0.0, kMaxSleep);
}

}   // namespace syspoll
