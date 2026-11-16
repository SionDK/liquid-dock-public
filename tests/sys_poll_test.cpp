// When the background poller reads volume, network and battery (src/sys_poll.h): the volume control is
// opened once per default device, read every 0.5 s while it does not report changes and every 15 s as a
// check while it does; a new device, a lost control and a network change are handled at once, a network
// change is followed by reads every 2 s for 12 s (bursts of reports read at most once a second); the
// battery stays every 5 s; and over a simulated hour the poller wakes and calls the audio service a
// small fraction of what it used to.
// No Windows headers: builds with any C++20 compiler.
//   cl /std:c++20 /EHsc /utf-8 tests\sys_poll_test.cpp
//   g++ -std=c++20 tests/sys_poll_test.cpp -o sys_poll_test
#include "../src/sys_poll.h"
#include "portable_check.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace syspoll;

namespace {

// A run of the poller against a simulated Windows; events come at the given times and wake it.
struct Sim {
    bool deviceThere = true;     // a default playback device exists
    bool reports = true;         // its control accepts a change report registration
    std::vector<std::pair<double, int>> events;   // (time, 1 device / 2 network / 3 control lost)
    double start = 0;            // the uptime the run starts at (QueryPerformanceCounter seconds)
    double until = 3600;         // ...and how long it runs
    // counts
    int wakes = 0, binds = 0, bindFails = 0, volumeReads = 0, netReads = 0, batteryReads = 0;
    double maxGapVolume = 0, lastVolume = -1;
    double maxGapBattery = 0, maxGapNet = 0, lastBattery = -1, lastNet = -1;
    double maxSleep = 0, minSleep = 1e9;
    std::vector<double> bindTimes, netTimes;

    void Run(double deviceAppearsAt = -1) {
        State s;
        double now = start;
        size_t next = 0;
        Events pending;
        while (now <= start + until) {
            ++wakes;
            if (deviceAppearsAt >= 0 && now >= start + deviceAppearsAt && !deviceThere) { deviceThere = true; pending.device = true; }
            Pass p = Next(s, pending, now);
            pending = {};
            bool bound = false, reporting = false;
            if (p.bindAudio) {
                ++binds; bindTimes.push_back(now);
                bound = deviceThere; reporting = deviceThere && reports;
                if (!bound) ++bindFails;
            }
            if (p.readVolume) { ++volumeReads; if (lastVolume >= 0) maxGapVolume = std::max(maxGapVolume, now - lastVolume); lastVolume = now; }
            if (p.readNet) { ++netReads; netTimes.push_back(now); if (lastNet >= 0) maxGapNet = std::max(maxGapNet, now - lastNet); lastNet = now; }
            if (p.readBattery) { ++batteryReads; if (lastBattery >= 0) maxGapBattery = std::max(maxGapBattery, now - lastBattery); lastBattery = now; }
            double sleep = Done(s, p, now, bound, reporting);
            maxSleep = std::max(maxSleep, sleep); minSleep = std::min(minSleep, sleep);
            double wake = now + std::max(sleep, 0.001);
            if (next < events.size() && start + events[next].first < wake) {   // an event wakes it early
                wake = std::max(start + events[next].first, now);
                switch (events[next].second) {
                case 1: pending.device = true; break;
                case 2: pending.network = true; break;
                case 3: pending.volumeLost = true; break;
                }
                ++next;
            }
            now = wake;
        }
    }
};

}   // namespace

int main() {
    // ---- one pass at a time
    {
        State s;
        Pass p = Next(s, {}, 0);
        Check(p.bindAudio && p.readNet && p.readBattery && !p.readVolume, "first pass: open the volume control, read network and battery");
        double sleep = Done(s, p, 0, true, true);
        Check(std::fabs(sleep - kMaxSleep) < 1e-9, "the control reports changes: the poller sleeps until the battery is due (5 s)");
        p = Next(s, {}, 2.0);
        Check(!p.bindAudio && !p.readVolume && !p.readNet && !p.readBattery, "nothing is due before then");
        p = Next(s, {}, 5.0);
        Check(p.readBattery && !p.readVolume && !p.bindAudio && !p.readNet, "at 5 s: the battery only (no volume read, nothing opened again)");
        Done(s, p, 5.0, false, false);
        p = Next(s, {}, kVolumeCheck);
        Check(p.readVolume && !p.bindAudio, "a control that reports is still read every kVolumeCheck (its reports can stop without a word)");
        Done(s, p, kVolumeCheck, false, false);
        Check(s.bound && s.reporting && std::fabs(s.nextVolume - 2 * kVolumeCheck) < 1e-9, "...a read that went well keeps it as it is");
        Events missed; missed.volumeLost = true;   // the read failed, or showed a change it had not reported
        Check(Next(s, missed, kVolumeCheck + 0.01).bindAudio, "...one that failed or missed a change opens it again at once");
    }
    {
        State s;
        Pass p = Next(s, {}, 0);
        double sleep = Done(s, p, 0, true, false);
        Check(std::fabs(sleep - kVolumePoll) < 1e-9, "no change reports: the open control is read every 0.5 s");
        p = Next(s, {}, 0.5);
        Check(p.readVolume && !p.bindAudio, "...read, not opened again");
    }
    {
        State s;
        Pass p = Next(s, {}, 0);
        double sleep = Done(s, p, 0, false, false);
        Check(std::fabs(sleep - kAudioRetry) < 1e-9 && !s.bound, "no playback device: looked for again after 5 s");
        Check(!Next(s, {}, 1.0).bindAudio, "...not before");
        Events e; e.device = true;
        Check(Next(s, e, 1.0).bindAudio, "...unless a device comes: then at once");
    }
    {
        State s;
        Pass p = Next(s, {}, 0);
        Done(s, p, 0, true, true);
        Events e; e.device = true;
        p = Next(s, e, 10);
        Check(p.bindAudio, "a new default device: its control is opened at once");
        Events lost; lost.volumeLost = true;
        Done(s, p, 10, true, false);
        Check(Next(s, lost, 10.2).bindAudio, "a control that can no longer be read is opened again at once");
        Events net; net.network = true;
        Check(Next(s, net, 10.3).readNet, "a network change is read at once");
    }
    {   // a reported network change: read at once, then every kNetFollow for kNetSettle, then kNetSafety again
        State s;
        Pass p = Next(s, {}, 0);
        Done(s, p, 0, true, true);
        Events net; net.network = true;
        p = Next(s, net, 100);
        Check(p.readNet, "Wi-Fi reconnects: read at once (the link is up, the address may not be yet)");
        double sleep = Done(s, p, 100, false, false);
        Check(sleep <= kNetFollow + 1e-9 && std::fabs(s.nextNet - (100 + kNetFollow)) < 1e-9, "...read again kNetFollow later");
        std::vector<double> reads;
        for (double t = 100.5; t < 180; t += 0.5) {
            Pass q = Next(s, {}, t);
            if (q.readNet) reads.push_back(t);
            Done(s, q, t, false, false);
        }
        int within = 0; double lastIn = 0, firstAfter = -1;
        for (double t : reads) { if (t <= 100 + kNetSettle + 1e-9) { ++within; lastIn = t; } else if (firstAfter < 0) firstAfter = t; }
        Check(within >= (int)(kNetSettle / kNetFollow) - 1 && within <= (int)(kNetSettle / kNetFollow) + 1,
              "...every 2 s while it settles (the address, the gateway, Windows' connectivity check: shown within 2 s, was 3 s before mod.15)");
        Check(firstAfter >= lastIn + kNetSafety - 1e-9, "...and then every kNetSafety again");
    }
    {   // a burst of reports (a VPN or virtual adapters): at most a read a second, the last state still read
        State s;
        Pass p = Next(s, {}, 0);
        Done(s, p, 0, true, true);
        int reads = 0; double lastRead = -1;
        for (double t = 50; t < 52; t += 0.05) {   // a report every 50 ms for 2 s
            Events net; net.network = true;
            Pass q = Next(s, net, t);
            if (q.readNet) { ++reads; lastRead = t; }
            Done(s, q, t, false, false);
        }
        Check(reads == 2 && lastRead >= 51.0 - 1e-9, "40 reports in 2 s: a read at the first and one a second later, not 40");
        Check(s.nextNet <= lastRead + kNetGap + 1e-9, "...and the state after the burst is read kNetGap after the last read");
    }

    // ---- a simulated hour, steady: the control reports, nothing changes
    {
        Sim sim; sim.Run();
        Check(sim.binds == 1, "an hour: the volume control is opened once (it was 7200 times)");
        Check(sim.volumeReads <= 3600 / kVolumeCheck + 1 && sim.maxGapVolume <= kVolumeCheck + 1e-6,
              "...and read every 15 s as a check (240 reads of two values; it was opened and read 7200 times)");
        Check(sim.wakes <= 3600 / 5 + 2, "...the poller wakes about every 5 s (it was every 0.5 s)");
        Check(sim.netReads <= 3600 / 30 + 2 && sim.maxGapNet <= kNetSafety + 1e-6, "...the network every 30 s at most (it was every 3 s)");
        Check(sim.maxGapBattery <= kBattery + 1e-6 && sim.batteryReads >= 3600 / 5, "...the battery still every 5 s");
        Check(sim.maxSleep <= kMaxSleep + 1e-9 && sim.minSleep >= 0, "sleeps stay between 0 and kMaxSleep");
        std::printf("steady hour: %d wakes, %d opens, %d volume reads, %d network reads, %d battery reads\n",
                    sim.wakes, sim.binds, sim.volumeReads, sim.netReads, sim.batteryReads);
    }
    // ---- the control does not report: polled, never reopened
    {
        Sim sim; sim.reports = false; sim.until = 600; sim.Run();
        Check(sim.binds == 1 && sim.volumeReads >= 600 / kVolumePoll - 2, "no change reports: read every 0.5 s, opened once (it was opened every time)");
    }
    // ---- events: a headset plugged in, the network switching, the device lost
    {
        Sim sim;
        sim.events = { { 100.2, 1 }, { 250.7, 2 }, { 400.1, 3 }, { 900.05, 2 } };
        sim.until = 1200;
        sim.Run();
        bool rebound = false, renet = false, lostRebound = false, renet2 = false;
        for (double t : sim.bindTimes) { if (std::fabs(t - 100.2) < 1e-6) rebound = true; if (std::fabs(t - 400.1) < 1e-6) lostRebound = true; }
        for (double t : sim.netTimes) { if (std::fabs(t - 250.7) < 1e-6) renet = true; if (std::fabs(t - 900.05) < 1e-6) renet2 = true; }
        Check(rebound, "a new default device is taken at the moment it is reported");
        Check(lostRebound, "a lost control is opened again at once");
        Check(renet && renet2, "network changes are read at the moment they are reported");
        Check(sim.binds == 3, "...and only then is anything opened again");
        Check(sim.netReads <= 1200 / 30 + 2 * (int)(kNetSettle / kNetFollow + 2) + 2, "...two network changes cost a dozen extra reads, no more");
    }
    // ---- no device at logon, one comes 42 s later
    {
        Sim sim; sim.deviceThere = false; sim.until = 120;
        sim.Run(42.0);
        Check(sim.bindFails >= 1 && sim.bindFails <= (int)(42 / kAudioRetry) + 1, "no device yet: looked for every 5 s, not every pass");
        bool at42 = false;
        for (double t : sim.bindTimes) if (t >= 42.0 && t < 42.0 + kAudioRetry + 1e-6) at42 = true;
        Check(at42, "the device that comes is opened within kAudioRetry");
    }
    // ---- after a year of uptime: the same schedule as after logon (doubles keep milliseconds there)
    {
        Sim a; a.Run();
        Sim b; b.start = 365.0 * 24 * 3600; b.Run();
        Check(a.wakes == b.wakes && a.binds == b.binds && a.volumeReads == b.volumeReads && a.netReads == b.netReads && a.batteryReads == b.batteryReads,
              "a year of uptime: the same wakes and reads per hour as right after logon");
    }
    // ---- sleep and resume: the clock jumps 8 hours between two passes
    {
        State s;
        double now = 0;
        for (; now < 100; ) { Pass p = Next(s, {}, now); now += std::max(Done(s, p, now, true, true), 0.001); }
        const double resumed = now + 8 * 3600;
        Pass p = Next(s, {}, resumed);
        Check(p.readBattery && p.readNet && p.readVolume && !p.bindAudio, "resume after 8 h: battery, network and the volume check are due at once, nothing reopened");
        double sleep = Done(s, p, resumed, false, false);
        Check(sleep >= kBattery - 1e-9, "...once: the next pass is the usual 5 s later, not a run of catch-up passes");
        p = Next(s, {}, resumed + sleep);
        Check(p.readBattery && !p.readNet && !p.readVolume, "...then only the battery, as before the sleep");
    }
    // ---- a second network report while it settles: the settling runs 12 s from the last report
    {
        State s;
        Pass p = Next(s, {}, 0);
        Done(s, p, 0, true, true);
        Events net; net.network = true;
        for (double t : { 200.0, 208.0 }) { Pass q = Next(s, net, t); Done(s, q, t, false, false); }
        Check(std::fabs(s.settleUntil - (208 + kNetSettle)) < 1e-9, "a report 8 s into the settling: it settles until 12 s after that one");
        double lastFollow = 0;
        for (double t = 208.5; t < 300; t += 0.5) {
            Pass q = Next(s, {}, t);
            if (q.readNet && t < 208 + kNetSettle + kNetFollow + 1e-9) lastFollow = t;
            Done(s, q, t, false, false);
        }
        Check(lastFollow >= 208 + kNetSettle - kNetFollow - 1e-9, "...and is read every 2 s up to then");
    }
    // ---- a device change and a network change in the same pass: both handled, the sleep that follows is short
    {
        State s;
        Pass p = Next(s, {}, 0);
        Done(s, p, 0, true, true);
        Events both; both.device = true; both.network = true;
        p = Next(s, both, 40);
        Check(p.bindAudio && p.readNet, "a headset and a network change together: the control is opened again and the network read");
        double sleep = Done(s, p, 40, true, true);
        Check(sleep <= kNetFollow + 1e-9 && s.bound && s.reporting, "...and the follow-up read of the network comes within 2 s");
    }
    // ---- the device stays away: looked for every 5 s, never more often
    {
        Sim sim; sim.deviceThere = false; sim.until = 600; sim.Run();
        Check(sim.binds <= 600 / kAudioRetry + 2 && sim.binds >= 600 / kAudioRetry - 2 && sim.wakes <= 600 / 5 + 3,
              "no playback device for 10 minutes: one look every 5 s, no extra wakes");
    }
    return Summary("sys_poll_test");
}
