#pragma once
#include "common.h"

// The Start button of Explorer's own taskbar, pressed through UI Automation: a click on the dock's start
// button is then what a click on the taskbar's is - Explorer handles it, with all it does around it (which
// monitor, the keyboard, the animation, a second press closing Start). start_menu.cpp decides when
// (start_menu_logic.h): first, and checked; the shell's request and the Windows key stay the fallback.
//
// The taskbar stays while the dock runs, invisible (NativeTaskbar, system.h). On Windows 11 its buttons are
// XAML, reachable only through UI Automation - and perhaps only while the taskbar is expanded: native_tray.cpp
// found the tray buttons gone from the automation tree once it auto-hides, an element found before still
// working. So each taskbar's Start button is looked up just before the taskbar vanishes (Remember) and kept;
// a press with none kept looks again, from the taskbar and from its XAML island's windows - not again within
// 30 s of a lookup that found nothing, twice as long after each further miss (each lookup costs calls into
// Explorer). Windows 10's classic taskbar has no such button: it is never looked into.
// (tests\run_taskbar_probe.bat shows what a given Windows offers.)
//
// UI Automation runs here only on threads of the process's multithreaded apartment, kept alive for the life
// of the process: the elements found belong to the apartment that found them (Microsoft's threading advice
// for UI Automation clients: an MTA thread that owns no windows). Focus is not moved to the hidden taskbar
// before a press (AutoSetFocus off): a focused auto-hide taskbar slides in.
namespace taskbarstart {

// Explorer's taskbar on this monitor: its main one on the primary monitor, a secondary one elsewhere, nullptr
// when there is none ("show my taskbar on all displays" off) or Explorer is not up. Any thread.
HWND TaskbarOn(HMONITOR monitor);

// Look up this taskbar's Start button and keep it - a kept one is asked whether it is still there (Explorer
// can rebuild its taskbar's content in the same window), at most every 10 s - unless a lookup runs or one
// found nothing lately. NativeTaskbar calls it while the taskbar is still expanded, just before it vanishes.
// Any thread; waits on Explorer for at most 0.8 s - not at all when there is nothing to do, or when such a
// wait ran out moments ago (Explorer busy: the lookup goes on without the caller).
void Remember(HWND taskbar);

enum class Found : uint8_t { Kept, Now, NoTaskbar, NoButton, Classic };
const wchar_t* FoundText(Found f);

// Is there a Start button to press for this monitor: kept, or found now (which waits on Explorer: tens of
// milliseconds, about a second when it is slow, a few seconds when it hangs)? MTA threads only (the start
// button's worker).
Found Find(HMONITOR monitor);

// A Start button is kept for this monitor's taskbar. Any thread; no call into Explorer.
bool Kept(HMONITOR monitor);

struct Pressed {
    bool ok = false;            // the press went through: the button's pattern answered S_OK
    bool unsure = false;        // it failed in a way that may still reach Explorer (a timeout, Explorer gone
                                // mid-call): never followed by another press, the shell or the key
    HRESULT hr = E_FAIL;
    const wchar_t* how = L"";   // "invoke", "toggle", "default action" - or why there was no press
    double took = 0;            // seconds the press call itself took (slow: Explorer is busy)
};
// Press the Start button of this monitor's taskbar (kept, or found now). A button that is gone (Explorer
// rebuilt its taskbar) was surely not pressed: it is dropped and looked up once more. MTA threads only.
Pressed Press(HMONITOR monitor);

// Windows 10's taskbar (or ExplorerPatcher's classic one): no XAML Start button to press. Any thread.
bool Classic();

// For the log and the probe: what is kept or was missed, per taskbar.
std::wstring Describe();

}   // namespace taskbarstart
