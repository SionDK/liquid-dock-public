#pragma once
#include "common.h"
#include <uiautomation.h>

// What the dock's calls into Explorer's UI Automation share (taskbar_start.cpp for the Start button, taskbar_apps.cpp
// for the app buttons, native_tray.cpp for the tray's): the per-call timeout and what a failed call means. Nothing here
// calls Windows or COM - a test's fake (#define before it includes the .cpp) reaches all of their calls, whichever
// header came first. The rest stays with each: UI Automation's object (native_tray.cpp leaves AutoSetFocus on), the
// lookups, telling a classic taskbar, and how a press's failure is classified (each its own way).
namespace exploreruia {

constexpr DWORD kUiaTimeoutMs = 500;     // per call into a busy Explorer (connection, transaction)
constexpr HRESULT kUiaTimeout = static_cast<HRESULT>(0x80131505);   // UIA_E_TIMEOUT (not in every SDK's headers)

// The element was surely not reached: it is gone (Explorer rebuilt its taskbar, or restarted). Look again.
inline bool Gone(HRESULT hr) {
    return hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) || hr == RPC_E_DISCONNECTED || hr == RPC_E_SERVER_DIED_DNE ||
           hr == CO_E_OBJNOTCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED_DNE);
}
// The call failed, and may have run all the same (the call timed out, or Explorer went away during it).
inline bool MayHaveRun(HRESULT hr) {
    return hr == kUiaTimeout || hr == RPC_E_TIMEOUT || hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) || hr == RPC_E_SERVER_DIED ||
           hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED);
}

}   // namespace exploreruia
