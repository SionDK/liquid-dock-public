#pragma once
// The fake window system of dock_interaction_test and dock_render_device_test: fake monitors (monitors) and one
// fake window in front (fg*), for the Dock's part files included after it (dock_fixture_parts.h names these fakes
// in its #define list). Every other window and monitor goes to the real call. Include it after the test's src\ and
// Windows headers. Each test sets monitors and fg* itself.
struct MonitorCase { HMONITOR handle; RECT rect; bool primary; bool actual; const char* name; };
static std::vector<MonitorCase> monitors;
static bool fgEnabled=false, fgMaximized=false;
static RECT fgRect{};
static HMONITOR fgMonitor=nullptr;
static LONG_PTR fgStyle=WS_POPUP;
static const wchar_t* fgClass=L"MatrixFullscreen";
static HWND fgHandle=(HWND)(uintptr_t)0xF00DFACE;
static BOOL WINAPI MatrixMonitorInfo(HMONITOR m,LPMONITORINFO info) {
 for(const auto& entry:monitors) if(entry.handle==m) {info->rcMonitor=info->rcWork=entry.rect;info->dwFlags=entry.primary?MONITORINFOF_PRIMARY:0;return TRUE;}
 return ::GetMonitorInfoW(m,info);
}
static HWND WINAPI MatrixForeground(){return fgEnabled?fgHandle:nullptr;}
static HMONITOR WINAPI MatrixMonitorFromWindow(HWND h,DWORD flags){return h==fgHandle ? fgMonitor : ::MonitorFromWindow(h,flags);}
static BOOL WINAPI MatrixWindowRect(HWND h,LPRECT rect){if(h==fgHandle){*rect=fgRect;return TRUE;}return ::GetWindowRect(h,rect);}
static BOOL WINAPI MatrixIsZoomed(HWND h){return h==fgHandle ? fgMaximized : ::IsZoomed(h);}
static LONG_PTR WINAPI MatrixWindowLong(HWND h,int index){return h==fgHandle ? fgStyle : ::GetWindowLongPtrW(h,index);}
// the client area GetWindowInfo reports for the window in front (a maximized window is fullscreen only when it
// covers the monitor): empty unless a check sets it; fgInfoFails makes the call fail
static RECT fgClient{};
static bool fgInfoFails=false;
static BOOL WINAPI MatrixWindowInfo(HWND h,PWINDOWINFO wi){
 if(h!=fgHandle)return ::GetWindowInfo(h,wi);
 if(fgInfoFails)return FALSE;
 wi->rcWindow=fgRect;wi->rcClient=fgClient;wi->dwStyle=(DWORD)fgStyle;return TRUE;
}
static int WINAPI MatrixClassName(HWND h,LPWSTR text,int capacity){if(h==fgHandle){wcsncpy_s(text,capacity,fgClass,_TRUNCATE);return (int)wcslen(text);}return ::GetClassNameW(h,text,capacity);}
static BOOL WINAPI MatrixShowWindow(HWND,int){return TRUE;} // keep fixture windows invisible
static SHORT WINAPI MatrixKeyState(int){return 0;}
static BOOL WINAPI MatrixForegroundSet(HWND){return TRUE;}
