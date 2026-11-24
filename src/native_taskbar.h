#pragma once
#include "common.h"
#include <atomic>
#include <mutex>

// Hides the native Windows taskbar while the dock runs and restores it afterwards (native_taskbar.cpp).
class NativeTaskbar {
public:
    bool Hide();
    bool Restore();             // false keeps the saved state available for a later retry
    // Re-hide if explorer showed it again. The periodic check runs on its own thread
    // (StartEnforcer) because explorer calls can block for seconds after logon or resume.
    // checkAutoHide: also ask Explorer whether its taskbar still auto-hides (a round trip to it).
    void Enforce(bool onUiThread = true, bool checkAutoHide = true);
    void StartEnforcer();
    void RecheckSoon() { recheck = true; }   // taskbar settings changed: the enforcer asks on its next pass
    bool WithInvisibleTaskbars(const std::function<bool()>& action);
    std::atomic<bool> hidden{ false };
private:
    std::atomic<bool> recheck{ false };
    std::recursive_mutex mu;
    UINT_PTR originalState = 0;
    bool stateSaved = false;
};
extern NativeTaskbar g_taskbar;
// Where NativeTaskbar keeps the taskbar's state from before the dock (ABM_GETSTATE) while it is hidden, for a
// restore after a crash (the watchdog copy, main_instance.cpp, reads it too): HKCU\<key>, value <value>.
// Macros: the same literal at each use.
#define LD_TASKBAR_STATE_KEY L"Software\\LiquidDock"
#define LD_TASKBAR_STATE_VALUE L"TaskbarState"
constexpr UINT WM_APP_ENFORCE = WM_APP + 30;   // to the main window (main.cpp): Enforce() on the UI thread
