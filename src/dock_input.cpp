#include "dock.h"
#include "system.h"
#include "sensors.h"
#include "native_tray.h"
#include "start_menu.h"
#include <shellapi.h>

// ---------------------------------------------------------------- input

// what is under a mouse message's point (client coordinates in lp)
Dock::Hit Dock::HitAtClient(LPARAM lp) {
    float u, v; ClientToUV(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), u, v);
    return HitTestUV(u, v);
}

void Dock::Click(const Hit& h) {
    switch (h.kind) {
    case HitSlot: {
        if (h.slot < 0 || h.slot >= (int)slots.size()) return;
        auto& s = slots[h.slot];
        if (!s.app) { startmenu::Click(monitor, startInFrontAtPress); return; }   // the start button (start_menu.h)
        auto app = s.app;
        if (g_settings.shiftRunsAsAdmin && (GetKeyState(VK_SHIFT) & 0x8000)) g_apps.Launch(*app, index, true);
        else g_apps.Click(*app, index, monitor);
        break;
    }
    // Windows' own tray windows rather than re-implementations
    case HitTray: {
        float u = cells[HitTray].Mid();
        nativetray::OverflowFailure why = nativetray::OverflowFailure::None;
        if (nativetray::OpenOverflow(ScreenPointUV(u, margin + barThick * 0.5f), g_settings.edge, BarScreenRect(), &why)) break;
        if (why == nativetray::OverflowFailure::NoButton) {
            if (MessageBoxW(hwnd, L"Windows 작업 표시줄에 숨겨진 아이콘 단추가 없습니다. 숨겨진 아이콘이 없거나, 작업 표시줄 설정의 "
                                  L"'숨겨진 아이콘 메뉴'가 꺼져 있을 때 그렇습니다.\n\n작업 표시줄 설정을 열까요?",
                            L"Liquid Dock", MB_YESNO | MB_ICONINFORMATION) == IDYES)
                ShellExecuteW(nullptr, L"open", L"ms-settings:taskbar", nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            MessageBoxW(hwnd, L"숨겨진 아이콘 창을 열지 못했습니다. 잠시 후 다시 시도해 주세요.", L"Liquid Dock", MB_OK | MB_ICONINFORMATION);
        }
        break;
    }
    case HitClock: nativetray::OpenCalendar(); break;
    case HitBattery: nativetray::OpenQuickSettings(); break;   // as the battery icon of the taskbar
    case HitSensor: case HitPanel: {
        // the source HWiNFO, its CPU temperature wanted (or, a click on the panel, in the power view 그래프 its package
        // power: the cell shows no power), HWiNFO up without it: only its Gadget setting is missing ("HWiNFO 설정: 클릭" /
        // "HWiNFO 전력: 클릭" on the card). Anything else: Task Manager (the source is chosen in the settings or the
        // cell's menu, never by a click here).
        const sensorlogic::ShowItems show = g_sensors.Items();
        if (sensorlogic::HWiNFOGuideWanted(show, g_sensors.CpuTempAvailable(), g_sensors.HWiNFORunning(), g_sensors.Power(), h.kind == HitPanel))
            g_sensors.ShowHWiNFOGuide();
        else OpenTaskManager();
        break;
    }
    case HitButton:
        if (h.button != ButtonId::Count) (this->*Button(h.button).click)();
        break;
    case HitNotify: NotifyClick(h.notify, trayhost::Mouse::LeftClick); break;
    default: break;
    }
}

void Dock::EndDrag(bool commit) {
    if (!dragging) return;
    dragging = false;
    if (pressSlot >= 0 && pressSlot < (int)slots.size() && commit) {
        auto app = slots[pressSlot].app;
        if (app) {
            if (dropRemove) g_apps.SetPinned(*app, false);
            else {
                int pcOthers = g_apps.PinnedCount() - (app->pinned ? 1 : 0);
                g_apps.MoveTo(*app, dropIndex, dropIndex <= pcOthers || app->pinned);
            }
        }
    }
    for (auto& s : slots) { s.gapBefore.target = 0; s.shrink.target = 1; }
    trailingGap.target = 0;
    pressSlot = -1;
    dirty = true;
}

void Dock::ClearPress() {
    pressSlot = -1; pressKind = HitNone; pressButton = ButtonId::Count; pressNotifyKey = 0;
    pressUpSince = -1;
    for (auto& s : slots) s.press.target = 0;
    dirty = true;
}

LRESULT Dock::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    // (&& msg: 0 when the registration failed - WM_NULL, which PopupMenu sends after a menu)
    if (msg == TaskbarCreatedMessage() && msg) { OnTaskbarCreated(); return 0; }
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_LBUTTONDOWN: {
        windowPreview.Dismiss();
        Hit h = HitAtClient(lp);
        pressKind = h.kind; pressSlot = h.kind == HitSlot ? h.slot : -1; pressButton = h.button;
        pressNotifyKey = h.kind == HitNotify ? NotifyKey(h.notify) : 0;
        // before the click can change what is in front: is it Start (the click then closes it)?
        startInFrontAtPress = pressSlot >= 0 && pressSlot < (int)slots.size() && !slots[pressSlot].app && startmenu::InFront();
        GetCursorPos(&pressPt);
        pressUpSince = -1;
        if (pressSlot >= 0) slots[pressSlot].press.target = 1;
        SetCapture(hwnd);
        dirty = true;
        return 0;
    }
    case WM_LBUTTONUP: {
        // the press as it was: giving the capture back (WM_CAPTURECHANGED) ends a drag and clears the press
        const int ps = pressSlot; const HitKind pk = pressKind; const ButtonId pb = pressButton; const uint64_t pn = pressNotifyKey;
        const bool wasDragging = dragging;
        pressKind = HitNone;   // the press ends here, not by losing the capture
        ReleaseCapture();
        if (dragging) EndDrag(true);   // the dock did not hold the capture: no WM_CAPTURECHANGED ended the drag
        ClearPress();
        if (wasDragging) return 0;
        Hit h = HitAtClient(lp);
        if (h.kind == pk && (h.kind != HitSlot || h.slot == ps) && (h.kind != HitButton || h.button == pb) &&
            (h.kind != HitNotify || NotifyKey(h.notify) == pn)) Click(h);
        return 0;
    }
    case WM_CAPTURECHANGED:
        // Another window took the capture (a menu opened with the right button while the left one is still
        // down): the button-up goes there, so the press is over now - kept, it would hold the dock up and
        // busy, and a later drag elsewhere on the screen would drag the icon. (A drag drops where it is.)
        if ((HWND)lp != hwnd && (dragging || pressKind != HitNone || pressSlot >= 0)) {
            if (dragging) EndDrag(true);
            else if (pressKind != HitNone) Log(L"input: dock %d lost the mouse capture during a press: no click", index);
            ClearPress();
        }
        return 0;
    case WM_RBUTTONUP: {
        windowPreview.Dismiss();
        Hit h = HitAtClient(lp);
        if (h.kind == HitNotify) NotifyClick(h.notify, trayhost::Mouse::RightClick);   // the program's own menu
        else if (h.kind == HitSlot && slots[h.slot].app) ShowAppMenu(slots[h.slot]);
        else if (h.kind == HitSlot && g_settings.startMenuExtras) ShowStartMenu();
        else if (h.kind == HitButton && Button(h.button).rightClick) (this->*Button(h.button).rightClick)();
        else if (h.kind == HitClock || h.kind == HitSensor || h.kind == HitPanel || h.kind == HitBattery) ShowTrayMenu(h);
        else ShowDockMenu();
        return 0;
    }
    case WM_MBUTTONUP: {
        windowPreview.Dismiss();
        Hit h = HitAtClient(lp);
        if (h.kind == HitSlot && slots[h.slot].app) { auto app = slots[h.slot].app; g_apps.Launch(*app, index); }
        else if (h.Is(ButtonId::Volume)) g_sys.ToggleMute();
        else if (h.kind == HitNotify) NotifyClick(h.notify, trayhost::Mouse::MiddleClick);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        float u, v; ScreenToUV(p, u, v);
        Hit h = HitTestUV(u, v);
        if (h.Is(ButtonId::Volume)) g_sys.AddVolume(GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 0.02f);
        return 0;
    }
    case WM_DROPFILES: {
        windowPreview.Dismiss();
        HDROP hd = (HDROP)wp;
        POINT p; DragQueryPoint(hd, &p);
        float u, v; ClientToUV(p.x, p.y, u, v);
        if (HitTestUV(u, v).Is(ButtonId::Trash)) { DropOnTrash(hd); DragFinish(hd); return 0; }
        int idx = 0;
        for (auto& s : slots) if (s.app && !s.ghost && s.u < u) idx++;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH * 2];
            if (DragQueryFileW(hd, i, path, ARRAYSIZE(path))) idx = g_apps.PinFile(path, idx) + 1;   // the next one right after it
        }
        DragFinish(hd);
        return 0;
    }
    case WM_APP_APPBAR:   // another app bar or the taskbar moved: claim the strip again
        if (wp == ABN_POSCHANGED) appBarRect = {};
        return 0;
    case WM_SETTINGCHANGE:   // the work area changed: does it still leave the strip out (CheckAppBar)
        if (wp == SPI_SETWORKAREA) workAreaChanged = true;
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); dirty = true; return 0; }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
