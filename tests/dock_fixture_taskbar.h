#pragma once
// Link stubs of the dock tests (dock_interaction_test, dock_layout_test, dock_render_device_test,
// dock_render_settings_window_test) for the taskbar's own buttons. Include it after the test's src\ and Windows
// headers, where the test's other stubs are (it includes two src\ headers: keep it at the same place).
// The taskbar's Start button (native_taskbar.obj's taskbar hiding looks it up): the tests never look into the real taskbar
#include "../src/taskbar_start.h"
namespace taskbarstart { void Remember(HWND) {} }
// app clicks through the taskbar's own buttons (apps_press.obj, taskbar_apps.h): the tests never press the real
// taskbar - no press can be started, so every click goes the dock's own way
#include "../src/taskbar_apps.h"
#include "stub_taskbar_apps.h"
