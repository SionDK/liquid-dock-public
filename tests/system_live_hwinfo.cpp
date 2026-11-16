// HWiNFO's Gadget key on this PC, watched while the user changes HWiNFO's settings (tests\run_system_live.bat):
// how HWiNFO writes HKCU\Software\HWiNFO64\VSB (rewritten in place, or deleted and made again), how long it
// stays quiet, and what the dock's own reader (hwinfo::Reader, src/hwinfo.cpp) makes of it meanwhile. Only
// reads the key; sends no input and touches no window. One handle to the key is kept open for the whole run:
// a key HWiNFO deletes and makes again leaves that handle on a deleted key (ERROR_KEY_DELETED), the case in
// which the reader's full scan (ReadAll) never ended (it now stops at the first such answer). The reader runs
// on a thread of its own and is given 2 s per read; one that does not come back is reported, and the process
// ends it.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\system_live_hwinfo.cpp src\hwinfo.cpp /link advapi32.lib
//   system_live_hwinfo.exe [seconds]      (90 by default)
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include "../src/hwinfo.h"

namespace {

const wchar_t kKey[] = L"Software\\HWiNFO64\\VSB";
ULONGLONG g_start = 0;
double Since() { return (GetTickCount64() - g_start) / 1000.0; }

ULONGLONG U64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

// the dock's read, on a thread of its own: false when it did not come back within `ms`
bool ReadWithin(hwinfo::Reader& reader, hwinfo::CpuReading& r, DWORD ms) {
    struct Job { hwinfo::Reader* reader; hwinfo::CpuReading r; };
    auto* job = new Job{ &reader, {} };   // left to a reader that does not come back
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* j = static_cast<Job*>(p);
        j->r = j->reader->ReadCpu();
        return 0;
    }, job, 0, nullptr);
    if (!t) { delete job; return false; }
    const bool back = WaitForSingleObject(t, ms) == WAIT_OBJECT_0;
    if (back) { r = job->r; delete job; }
    CloseHandle(t);
    return back;
}

}   // namespace

int main(int argc, char** argv) {
    int seconds = argc > 1 ? std::atoi(argv[1]) : 90;
    if (seconds < 10) seconds = 10;
    g_start = GetTickCount64();
    std::printf("HWiNFO Gadget key, watched for %d s (read only). Meanwhile, in HWiNFO: Sensors > Configure Sensors >\n"
                "HWiNFO Gadget - untick and tick again \"Enable reporting to Gadget\", untick and tick a reported value,\n"
                "and close HWiNFO and start it again.\n", seconds);
    HKEY kept = nullptr;
    LSTATUS rc = RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &kept);
    std::printf("[%5.1f] key %s\n", Since(), rc == ERROR_SUCCESS ? "there (one handle kept open)" : "not there (HWiNFO never reported to the Gadget on this PC)");

    hwinfo::Reader reader;
    bool keptDeleted = false, wasThere = rc == ERROR_SUCCESS, readerStuck = false;
    DWORD lastValues = (DWORD)-1;
    ULONGLONG lastWrite = 0;
    double maxQuiet = 0, quietSince = Since();
    int deletions = 0, reads = 0, found = 0, writes = 0;
    hwinfo::CpuReading last{};
    for (int tick = 0; Since() < seconds; ++tick) {
        // the kept handle: still on the key HWiNFO writes?
        if (kept && !keptDeleted) {
            DWORD values = 0; FILETIME w{};
            rc = RegQueryInfoKeyW(kept, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &values, nullptr, nullptr, nullptr, &w);
            if (rc == ERROR_KEY_DELETED) {
                keptDeleted = true;
                wchar_t name[64]; DWORD cch = 64, type = 0; BYTE data[64]; DWORD cb = sizeof(data);
                LSTATUS e0 = RegEnumValueW(kept, 0, name, &cch, nullptr, &type, data, &cb);
                std::printf("[%5.1f] the key was DELETED under the kept handle (RegEnumValueW on it: %ld%s)\n", Since(), e0,
                            e0 == ERROR_KEY_DELETED ? " - the dock's full scan stops at that answer" : "");
            }
        }
        // a fresh look: there, how many values, last written when
        HKEY k = nullptr;
        const bool there = RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) == ERROR_SUCCESS;
        if (there != wasThere) {
            std::printf("[%5.1f] key %s\n", Since(), there ? "made again" : "gone");
            if (!there) ++deletions;
            wasThere = there;
        }
        if (there) {
            DWORD values = 0; FILETIME w{};
            if (RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &values, nullptr, nullptr, nullptr, &w) == ERROR_SUCCESS) {
                if (values != lastValues) { std::printf("[%5.1f] %lu values\n", Since(), values); lastValues = values; }
                if (U64(w) != lastWrite) {
                    if (lastWrite) { const double quiet = Since() - quietSince; if (quiet > maxQuiet) maxQuiet = quiet; }
                    lastWrite = U64(w); quietSince = Since(); ++writes;
                }
            }
            RegCloseKey(k);
        }
        // the dock's reader, once a second as on its sensor thread
        if (tick % 10 == 0 && !readerStuck) {
            hwinfo::CpuReading r;
            if (!ReadWithin(reader, r, 2000)) {
                readerStuck = true;
                std::printf("[%5.1f] the dock's reader did NOT come back within 2 s (an endless scan: the sensor thread would hang)\n", Since());
            } else {
                ++reads; found += r.found;
                if (r.running != last.running || r.found != last.found)
                    std::printf("[%5.1f] the dock would show: %s%s\n", Since(), r.found ? "a CPU temperature" : "no CPU temperature",
                                r.running ? " (HWiNFO taken as running)" : " (HWiNFO not taken as running)");
                last = r;
            }
        }
        Sleep(100);
    }
    if (kept) RegCloseKey(kept);
    std::printf("SUMMARY: %d writes seen, longest quiet %.1f s, key gone %d time(s), kept handle %s, reader %s (%d reads, %d with a temperature)\n",
                writes, maxQuiet, deletions, keptDeleted ? "left on a DELETED key" : "still on the key",
                readerStuck ? "STUCK" : "always came back", reads, found);
    std::fflush(stdout);
    ExitProcess(readerStuck ? 1 : 0);   // a stuck reader ends with the process
}
