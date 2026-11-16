#pragma once
#include "common.h"

// The dock's start button (what it decides: start_menu_logic.h). A click presses the Start button of
// Explorer's own, hidden taskbar (taskbar_start.h) - or, without one, asks the shell for Start on the dock's
// monitor - and watches, on a worker thread, whether Start comes; the Windows key is the last fallback.
// The UI thread never waits on Explorer here.
namespace startmenu {

// Start is open (IAppVisibility, or Start's window in front): a press on the button then closes it.
// Checked when the button goes down, before the click itself can change anything. UI thread.
bool InFront();

// The start button was clicked (UI thread). startWasInFront: InFront() at the press.
void Click(HMONITOR monitor, bool startWasInFront);

}   // namespace startmenu
