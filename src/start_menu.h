#pragma once
#include "common.h"

// The dock's start button (what it decides: start_menu_logic.h). A click asks the shell for Start on the
// dock's monitor and watches, on a worker thread, whether Start comes; when it does not, the Start button of
// Explorer's own, hidden taskbar is pressed (taskbar_start.h), then the Windows key - the last fallback.
// The UI thread never waits on Explorer here.
namespace startmenu {

// Start is open (IAppVisibility, or Start's window in front): a press on the button then closes it (the Windows key).
// Checked when the button goes down, before the click itself can change anything. UI thread.
bool InFront();

// The start button was clicked (UI thread). startWasInFront: InFront() at the press.
void Click(HMONITOR monitor, bool startWasInFront);

// The display came on (main.cpp: its power notification, or the pointer moved while it was taken as off), now =
// NowSeconds. For dock.log only: a click that went otherwise says how long ago (both misses so far came after a long rest).
void DisplayCameOn(double now);

}   // namespace startmenu
