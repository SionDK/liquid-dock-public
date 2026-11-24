#pragma once
#include "common.h"

// IAppVisibility (shobjidl_core.h, documented since Windows 8): whether Start is shown. Declared once, here, for
// start_menu.cpp (InFront, the click's worker), shell_front.cpp (LauncherVisible) and the live probes, so every
// compiler sees the same - and under names of its own: a program that also includes <shobjidl.h> (apps.cpp, put
// with start_menu.cpp into tests\start_ghost_live_test.cpp) sees the SDK's IAppVisibility and CLSID_AppVisibility.
// Declarations only: the code that makes and asks the object stays in each .cpp, where a test's fake CoCreateInstance
// (#define before it includes the .cpp) reaches it.
const CLSID kAppVisibility = { 0x7e5fe3d9, 0x985f, 0x4908, { 0x91, 0xf9, 0xee, 0x19, 0xf9, 0xfd, 0x15, 0x14 } };
const IID kAppVisibilityIid = { 0x2246ea2d, 0xcaea, 0x4444, { 0xa3, 0xc4, 0x6d, 0xe8, 0x27, 0xe4, 0x43, 0x13 } };
struct IAppVisibilityDecl : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAppVisibilityOnMonitor(HMONITOR, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLauncherVisible(BOOL* visible) = 0;
};
