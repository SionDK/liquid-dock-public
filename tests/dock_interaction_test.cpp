// Runs the production Dock implementation with controlled monitor/cursor/foreground inputs.
// Never sends keyboard or mouse input, starts apps, hides taskbars, or alters user data.
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/capture.h"
#include "../src/apps.h"
#include "../src/system.h"
#include "../src/sensor_shared.h"
#include "../src/native_tray.h"
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d2d1effects.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <map>
#include <limits>
#define private public
#include "../src/sensors.h"
#include "../src/dock.h"
#undef private

Settings g_settings;
HWND g_mainWnd = nullptr;
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
// Chrome maximized and then put in F11 (or a page's fullscreen video): still maximized, no caption, no resize frame
static const LONG_PTR kChromeF11=WS_OVERLAPPED|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_VISIBLE|WS_MAXIMIZE;
static const LONG_PTR kChromeMaximized=WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_MAXIMIZE;
static RECT Grown(const RECT& r,int by){return {r.left-by,r.top-by,r.right+by,r.bottom+by};}
static int WINAPI MatrixClassName(HWND h,LPWSTR text,int capacity){if(h==fgHandle){wcsncpy_s(text,capacity,fgClass,_TRUNCATE);return (int)wcslen(text);}return ::GetClassNameW(h,text,capacity);}
// The z-order over a dock (FrontTests, mod.32): the window in front lies above the dock while fgAboveDock; the dock
// going topmost again (BringToFront) puts it back over it. Only the window lent to the dock in FrontTests is seen.
static HWND frontDock=nullptr;
static bool fgAboveDock=false;
static int dockRaises=0;
static HWND WINAPI MatrixGetWindow(HWND h,UINT cmd){
 if(cmd==GW_HWNDPREV&&h&&h==frontDock)return fgAboveDock?fgHandle:nullptr;
 if(cmd==GW_HWNDPREV&&h==fgHandle)return nullptr;
 return ::GetWindow(h,cmd);
}
static BOOL WINAPI MatrixSetWindowPos(HWND h,HWND after,int x,int y,int cx,int cy,UINT flags){
 if(h&&h==frontDock&&after==HWND_TOPMOST){++dockRaises;fgAboveDock=false;}
 return ::SetWindowPos(h,after,x,y,cx,cy,flags);
}
static BOOL WINAPI MatrixShowWindow(HWND,int){return TRUE;} // keep fixture windows invisible
static SHORT WINAPI MatrixKeyState(int){return 0;}
static int settingsMenus=0;
static BOOL WINAPI MatrixForegroundSet(HWND){return TRUE;}
static BOOL WINAPI MatrixPopupMenu(HMENU,UINT,int,int,int,HWND,const RECT*){++settingsMenus;return 0;}
static int desktopActions=0;
namespace nativetray {
static bool MatrixToggleDesktop(){++desktopActions;return true;}
}
// OLED mode stubs: the matrix never changes the real wallpaper or desktop icons
static int oledActions=0; static bool oledOn=false;
namespace oled {
bool Active(){return oledOn;}
bool CoversMonitor(HMONITOR){return oledOn;}   // the matrix's OLED mode covers every monitor
bool Enable(HMONITOR){++oledActions;oledOn=true;return true;}
bool Disable(){++oledActions;oledOn=false;return true;}
void Reapply(){}
}
// the shape window never opens in the matrix
#include "../src/settings_window.h"
static int settingsWindows=0;
namespace settingswindow { void Show(HMONITOR){++settingsWindows;} void Close(){} bool IsOpen(){return false;} bool PreTranslate(MSG*){return false;} }
static int themeActions=0; static bool themeLight=false;
namespace theme {
bool IsLight(){return themeLight;}
bool Toggle(){++themeActions;themeLight=!themeLight;return true;}
}
// Shell, Recycle Bin and audio stubs: the matrix never opens pages, deletes files or switches devices
static int shellOpens=0; static std::wstring lastShellOpen;
static HINSTANCE WINAPI MatrixShellExecute(HWND,LPCWSTR,LPCWSTR file,LPCWSTR,LPCWSTR,INT){++shellOpens;lastShellOpen=file?file:L"";return (HINSTANCE)42;}
static std::vector<std::wstring> lastDeleted; static UINT lastDeleteFunc=0; static FILEOP_FLAGS lastDeleteFlags=0;
static int MatrixFileOperation(LPSHFILEOPSTRUCTW op){
 lastDeleted.clear();lastDeleteFunc=op->wFunc;lastDeleteFlags=op->fFlags;
 for(const wchar_t* p=op->pFrom;p&&*p;p+=wcslen(p)+1)lastDeleted.push_back(p);
 return 0;
}
static HRESULT WINAPI MatrixEmptyRecycleBin(HWND,LPCWSTR,DWORD){return S_OK;}
#include "../src/audio_devices.h"
static int audioSwitches=0;
namespace audio {
std::vector<Device> Outputs(){return {{L"a",L"Speakers",true},{L"b",L"Headset",false}};}
bool SetDefault(const std::wstring&){++audioSwitches;return true;}
std::wstring DefaultOutputName(){return L"Speakers";}   // the audio button's tooltip (since mod.15)
}
// start button stubs: the matrix never opens Start or presses the Windows key
#include "../src/start_menu.h"
static int startClicks=0; static HMONITOR startClickMonitor=nullptr; static bool startClickInFront=false;
namespace startmenu {
bool InFront(){return false;}
void Click(HMONITOR m,bool startWasInFront){++startClicks;startClickMonitor=m;startClickInFront=startWasInFront;}
}
// the taskbar's Start button (system_taskbar.obj's taskbar hiding looks it up): the matrix never looks into the real taskbar
#include "../src/taskbar_start.h"
namespace taskbarstart { void Remember(HWND){} }
// app clicks through the taskbar's own buttons (apps_press.obj, taskbar_apps.h): the matrix never presses the real
// taskbar - no press can be started, so every click goes the dock's own way
#include "../src/taskbar_apps.h"
namespace taskbarapps {
bool Begin(const Job&){return false;}
bool Take(uint32_t,Result&){return false;}
const wchar_t* OutcomeText(Outcome){return L"";}
}
#define ShellExecuteW MatrixShellExecute
#define SHFileOperationW MatrixFileOperation
#define SHEmptyRecycleBinW MatrixEmptyRecycleBin
#define GetMonitorInfoW MatrixMonitorInfo
#define GetForegroundWindow MatrixForeground
#define MonitorFromWindow MatrixMonitorFromWindow
#define GetWindowRect MatrixWindowRect
#define IsZoomed MatrixIsZoomed
#define GetWindowLongPtrW MatrixWindowLong
#define GetWindowInfo MatrixWindowInfo
#define GetClassNameW MatrixClassName
#define ShowWindow MatrixShowWindow
#define GetAsyncKeyState MatrixKeyState
#define SetForegroundWindow MatrixForegroundSet
#define TrackPopupMenu MatrixPopupMenu
#define ToggleDesktop MatrixToggleDesktop
#define GetWindow MatrixGetWindow
#define SetWindowPos MatrixSetWindowPos
#include "../src/dock.cpp"
#include "../src/dock_window.cpp"
#include "../src/dock_input.cpp"
#include "../src/dock_layout.cpp"
#include "../src/dock_render.cpp"
#include "../src/dock_sensors.cpp"
#include "../src/dock_controls.cpp"
#include "../src/dock_notify.cpp"
#undef GetMonitorInfoW
#undef GetForegroundWindow
#undef MonitorFromWindow
#undef GetWindowRect
#undef IsZoomed
#undef GetWindowLongPtrW
#undef GetWindowInfo
#undef GetClassNameW
#undef ShowWindow
#undef GetAsyncKeyState
#undef SetForegroundWindow
#undef TrackPopupMenu
#undef ToggleDesktop
#undef GetWindow
#undef SetWindowPos

static int checks=0, failures=0, cases=0, caseFailures=0;
static std::string caseLabel;
static std::ofstream report;
static double simTime=10;
static void Check(bool ok,const char* name) {
 ++checks;
 if(!ok){++failures;++caseFailures;report<<"FAIL\t"<<caseLabel<<"\t"<<name<<"\n";if(failures<=25)printf("FAIL %s: %s\n",caseLabel.c_str(),name);}
}
static POINT EdgePoint(const RECT& r,Edge edge,float fraction,int depth) {
 LONG alongX=r.left+(LONG)((r.right-r.left-1)*fraction),alongY=r.top+(LONG)((r.bottom-r.top-1)*fraction);
 switch(edge){case Edge::Bottom:return {alongX,r.bottom-1-depth};case Edge::Top:return {alongX,r.top+depth};case Edge::Left:return {r.left+depth,alongY};default:return {r.right-1-depth,alongY};}
}
static POINT Outside(const RECT& r){return {r.left-1200,r.top-1200};}
static void Advance(Dock& d,POINT cursor,double seconds,float dt=1.f/120.f) {
 for(int i=0;i<(int)std::ceil(seconds/dt);++i){simTime+=dt;d.Update(simTime,dt,cursor);}
}
static void ResetHidden(Dock& d) {
 fgEnabled=false;fgMaximized=false;fgStyle=WS_POPUP;fgClass=L"MatrixFullscreen";fgClient={};fgInfoFails=false;
 d.reveal.snap(0);d.magAmount.snap(0);d.pinSide=0;d.pinBlend.snap(0);d.pinOff=0;d.panelAlpha.snap(0);d.tooltipAlpha.snap(0);
 d.edgeSince=d.outsideSince=d.panelOutsideSince=-1;d.menuOpen=false;d.dragging=false;d.pressSlot=-1;
 for(auto& s:d.slots){s.bounceCount=0;s.bounceLoop=false;s.peek.snap(0);s.bounceOff=0;}
 d.sbCount=0;d.sbPeek.snap(0);d.nextCaptureTry=std::numeric_limits<double>::max();
 Dock::debugForceShow=Dock::debugForcePanel=false;
 Advance(d,Outside(d.mon),0.2);
}
static void Apps(int count) {
 g_apps.items.clear();g_apps.attentionEvents.clear();++g_apps.structureVersion;
 for(int i=0;i<count;++i){auto item=std::make_shared<AppItem>();item->id=i+1;item->name=L"Test App "+std::to_wstring(i+1);item->pinned=true;g_apps.items.push_back(item);}
}
static void InitializeLayout(Dock& d) {
 d.Metrics();d.SyncSlots();
 for(auto& s:d.slots){s.appear.snap(1);s.shrink.snap(1);}
 d.trailingGap.snap(0);ResetHidden(d);
 Advance(d,Outside(d.mon),1.5);
}
static void AssertHitGeometry(Dock& d) {
 // Check what is drawn against the product's independent input classification and OS region coordinates.
 const float v=d.margin+d.pad+d.base*0.4f-d.hideOff;
 for(size_t i=0;i<d.slots.size();++i) {
  auto& slot=d.slots[i];auto px=d.W(slot.u,v);
  float u2,v2;d.ClientToUV((int)px.x,(int)px.y,u2,v2);
  auto hit=d.HitTestUV(u2,v2);
  Check(hit.kind==Dock::HitSlot && hit.slot==(int)i,"rendered icon center maps to its own hit target");
  POINT local={(LONG)px.x,(LONG)px.y};Check(PtInRect(&d.curRegion,local)!=0,"icon center inside active input region");
  Check(slot.u0>=-1 && slot.u0+slot.size*slot.weight<=d.len+1,"icons remain inside the monitor edge");
 }
 struct Item {float start,end;Dock::HitKind kind;};
 std::vector<Item> buttons={{d.cells[Dock::HitTray].u0,d.cells[Dock::HitTray].u1,Dock::HitTray},{d.cells[Dock::HitClock].u0,d.cells[Dock::HitClock].u1,Dock::HitClock},{d.cells[Dock::HitSensor].u0,d.cells[Dock::HitSensor].u1,Dock::HitSensor}};
 const bool battery=d.cells[Dock::HitBattery].u1>d.cells[Dock::HitBattery].u0+0.5f;
 if(battery)buttons.push_back({d.cells[Dock::HitBattery].u0,d.cells[Dock::HitBattery].u1,Dock::HitBattery});
 for(const auto& item:buttons){
  auto px=d.W((item.start+item.end)/2,d.margin+d.barThick/2-d.hideOff);
  float u2,v2;d.ClientToUV((int)px.x,(int)px.y,u2,v2);Check(d.HitTestUV(u2,v2).kind==item.kind,"tray control rendered center maps to its own hit target");
  Check(item.start>=-1 && item.end<=d.len+1,"tray controls remain inside monitor edge");
 }
 auto& netCell=d.Cell_(ButtonId::Network);auto& volCell=d.Cell_(ButtonId::Volume);
 auto network=d.ButtonRect(ButtonId::Network),volume=d.ButtonRect(ButtonId::Volume);
 if(d.stackButtons){
  Check(netCell.u0==volCell.u0 && netCell.u1==volCell.u1 && d.cells[Dock::HitClock].u0==volCell.u1,"stacked connectivity shares one cell before clock");
  if(IsVertical(g_settings.edge))Check(network.right<=volume.left+0.1f,"side docks: network left of volume");
  else Check(network.bottom<=volume.top+0.1f,"top and bottom docks: network above volume");
 }else Check(netCell.u1==volCell.u0 && d.cells[Dock::HitClock].u0==volCell.u1,"unstacked connectivity keeps two adjacent cells before clock");
 // every small button: inside the window, a comfortable size, its own click area, no overlap;
 // chosen buttons in the chosen order after the sensor cell, settings last
 std::vector<D2D1_RECT_F> seen;
 float lastU=-1e9f;
 for(int i=0;i<kButtonCount;++i){
  ButtonId id=(ButtonId)i;const auto& c=d.Cell_(id);
  if(!HasButton(id)){Check(c.u1<=c.u0,"buttons not on the dock take no space");continue;}
  auto r=d.ButtonRect(id);
  // half a pixel of slack: at gap 0 the glass is flush with the screen edge
  Check(r.left>=-0.5f && r.top>=-0.5f && r.right<=d.winW+0.5f && r.bottom<=d.winH+0.5f,"small buttons stay inside dock window");
  Check(std::min(r.right-r.left,r.bottom-r.top)>=22*d.scale-1,"small buttons keep a comfortable click size");
  Check(Info(id).fixed||c.u0>=d.cells[Dock::HitSensor].u1,"utility buttons follow the sensor cell");
  if(battery)Check(Info(id).fixed||c.u0>=d.cells[Dock::HitBattery].u1-0.01f,"utility buttons follow the battery cell");
  for(auto& o:seen){
   float ix=std::min(r.right,o.right)-std::max(r.left,o.left),iy=std::min(r.bottom,o.bottom)-std::max(r.top,o.top);
   Check(ix<=0.5f||iy<=0.5f,"small buttons never overlap");
  }
  seen.push_back(r);
  for(float x:{0.15f,0.5f,0.85f})for(float y:{0.15f,0.5f,0.85f}){
   POINT local={(LONG)lerpf(r.left,r.right,x),(LONG)lerpf(r.top,r.bottom,y)};
   float u,v;d.ClientToUV(local.x,local.y,u,v);
   Check(d.HitTestUV(u,v).Is(id),"small button drawing matches its click target across the cell");
   Check(PtInRect(&d.curRegion,local)!=0,"small button remains in active input region");
  }
 }
 {  // order: the user's list, then settings
  auto order=ChosenButtons();order.push_back(ButtonId::Settings);
  for(auto id:order){float u=d.Cell_(id).u0;Check(u>=lastU-0.01f,"buttons appear in the chosen order");lastU=u;}
 }
 // each optional button's click reaches its own action once (all stubbed: nothing real happens)
 if(HasButton(ButtonId::Oled)){
  int before=oledActions;d.Click(Dock::ButtonHit(ButtonId::Oled));d.Click(Dock::ButtonHit(ButtonId::Oled));
  Check(oledActions-before==2 && !oledOn,"OLED button toggles on and back off");
 }
 if(HasButton(ButtonId::Theme)){
  int before=themeActions;bool was=themeLight;d.Click(Dock::ButtonHit(ButtonId::Theme));d.Click(Dock::ButtonHit(ButtonId::Theme));
  Check(themeActions-before==2 && themeLight==was,"theme button flips the mode and back");
 }
 if(HasButton(ButtonId::Awake)){
  bool was=features::Awake();d.Click(Dock::ButtonHit(ButtonId::Awake));bool mid=features::Awake();d.Click(Dock::ButtonHit(ButtonId::Awake));
  Check(mid!=was && features::Awake()==was,"keep-awake button switches on and back off");
 }
 if(HasButton(ButtonId::Quiet)){
  bool was=features::Quiet(simTime);d.Click(Dock::ButtonHit(ButtonId::Quiet));bool mid=features::Quiet(simTime);d.Click(Dock::ButtonHit(ButtonId::Quiet));
  Check(mid!=was && features::Quiet(simTime)==was,"quiet button switches bounces off and back on");
 }
 for(auto [id,uri]:{std::pair{ButtonId::Screenshot,L"ms-screenclip:"},std::pair{ButtonId::Trash,L"shell:RecycleBinFolder"},
                      std::pair{ButtonId::Sff,L"https://gall.dcinside.com/mgallery/board/lists/?id=sff"}}){
  if(!HasButton(id))continue;
  int before=shellOpens;d.Click(Dock::ButtonHit(id));
  Check(shellOpens==before+1 && lastShellOpen==uri,"button opens its own page");
 }
 for(auto id:{ButtonId::Downloads,ButtonId::Audio}){
  if(!HasButton(id))continue;
  int before=settingsMenus;d.Click(Dock::ButtonHit(id));
  Check(settingsMenus==before+1,"list buttons open their menu");
 }
 {  // the start button hands the click to startmenu (stubbed) with this dock's monitor and what was in front at the press
  int start=-1;
  for(size_t i=0;i<d.slots.size();++i) if(!d.slots[i].app && !d.slots[i].ghost){start=(int)i;break;}
  Check(start>=0,"the dock has a start button");
  if(start>=0){
   Dock::Hit h;h.kind=Dock::HitSlot;h.slot=start;
   int before=startClicks;d.startInFrontAtPress=false;d.Click(h);
   Check(startClicks==before+1 && startClickMonitor==d.monitor && !startClickInFront,"start button: one request for Start on this dock's monitor");
   d.startInFrontAtPress=true;d.Click(h);
   Check(startClicks==before+2 && startClickInFront,"start button: Start in front at the press goes with the click (it closes Start)");
   d.startInFrontAtPress=false;
  }
 }
 Check(d.Cell_(ButtonId::Settings).u1-d.Cell_(ButtonId::Settings).u0>=32*d.scale-0.1f,"settings button retains its minimum target width in crowded layouts");
 {
  auto r=d.ButtonRect(ButtonId::Settings,true);
  POINT local={(LONG)((r.right+r.left)/2),(LONG)((r.top+r.bottom)/2)};
  Check(PtInRect(&d.curRegion,local)!=0,"settings target stays in active input region");
 }
 {
  // the end padding beside the gear (in the gear's half when it shares a cell)
  auto pr=d.ButtonRect(ButtonId::Settings,true);
  auto pe=d.W((d.Cell_(ButtonId::Settings).u1+d.barU1)*0.5f,d.margin+d.barThick*0.5f-d.hideOff);
  POINT pp=IsVertical(g_settings.edge)?POINT{(LONG)((pr.left+pr.right)*0.5f),(LONG)pe.y}:POINT{(LONG)pe.x,(LONG)((pr.top+pr.bottom)*0.5f)};
  float u,v;d.ClientToUV(pp.x,pp.y,u,v);
  Check(d.HitTestUV(u,v).Is(ButtonId::Settings),"dock-end padding also opens settings");
 }
 Check(d.curRegion.left>=0 && d.curRegion.top>=0 && d.curRegion.right<=d.winW && d.curRegion.bottom<=d.winH,"input region stays inside its window");
 Check(d.barU0>=-1 && d.barU1<=d.len+1,"glass bar stays inside monitor edge");
}
static void StateTests(Dock& d) {
 auto center=EdgePoint(d.mon,g_settings.edge,0.5f,0);
 // Only pixels inside this monitor count; its four corners and the shared monitor boundary matter.
 // The docks here have no window: these steps lend them one of the test's own, never shown, so the region
 // and the DWM cloak (Win+Shift+S) land on a real window.
 HWND own=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"dock_interaction_test",WS_POPUP,0,0,8,8,nullptr,nullptr,nullptr,nullptr);
 HWND lent=d.hwnd;if(!lent)d.hwnd=own;
 for(float fraction:{0.f,0.5f,1.f}) {
  ResetHidden(d);auto edge=EdgePoint(d.mon,g_settings.edge,fraction,0);
  Advance(d,edge,0.025);Check(d.reveal.target==0,"show delay prevents immediate reveal");
  Advance(d,edge,0.2);Check(d.reveal.x>0.5f,"edge and corner begin revealing after delay");
  {DWORD c=1;DwmGetWindowAttribute(d.hwnd,DWMWA_CLOAKED,&c,sizeof(c));Check(!d.cloaked && c==0,"a dock coming up is not cloaked");}
  int flips=0;float target=d.reveal.target;
  for(int frame=0;frame<240;++frame){Advance(d,edge,1.f/120.f);if(d.reveal.target!=target){++flips;target=d.reveal.target;}}
  Check(flips==0,"stationary edge pointer does not repeatedly toggle visibility");
  d.reveal.snap(1);d.outsideSince=-1;
  Advance(d,Outside(d.mon),0.12);Check(d.reveal.target==1,"hide delay keeps dock briefly visible");
  Advance(d,Outside(d.mon),1.6);Check(d.reveal.x<0.002f && IsRectEmpty(&d.curRegion),"leaving monitor hides dock and releases input region");
  {DWORD c=0;DwmGetWindowAttribute(d.hwnd,DWMWA_CLOAKED,&c,sizeof(c));Check(d.cloaked && c!=0,"...and cloaks its window (Win+Shift+S's window mode does not offer the empty strip)");}
 }
 if(!lent){d.hwnd=nullptr;d.cloaked=FALSE;d.curRegion={-1,-1,-1,-1};}
 if(own)DestroyWindow(own);
 for(int depth:{-1,3,60}){ResetHidden(d);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,depth),0.8);Check(d.reveal.target==0,"outside or merely near an edge does not reveal");}
 ResetHidden(d);Advance(d,center,0.8);AssertHitGeometry(d);
 for(size_t index:{(size_t)0,d.slots.size()-1}) {
  ResetHidden(d);Advance(d,center,0.8);
  auto p=d.ScreenPointUV(d.slots[index].u,d.margin+d.barThick/2);
  float u,v;d.ScreenToUV(p,u,v);
  auto initial=d.HitTestUV(u,v);
  Check(initial.kind==Dock::HitSlot && initial.slot==(int)index,"hover test starts on the rendered icon");
  bool keptTarget=true;
  for(int frame=0;frame<120;++frame){
   Advance(d,p,1.f/120.f);d.ScreenToUV(p,u,v);auto hit=d.HitTestUV(u,v);
   keptTarget &= hit.kind==Dock::HitSlot && hit.slot==(int)index;
  }
  Check(keptTarget,"first or last icon stays under pointer on every animation frame");
  if(d.reveal.target>0.5f && d.reveal.x>0.9f)AssertHitGeometry(d);
  Check(d.reveal.target==1,"hovering first or last icon keeps dock visible");
  if(d.reveal.target!=1)report<<"DETAIL\t"<<caseLabel<<"\ticon="<<index<<" cursorU="<<d.cursorU<<" cursorV="<<d.cursorV<<" bar="<<d.barU0<<","<<d.barU1<<" reveal="<<d.reveal.x<<"\n";
 }
 // The sensor popup must be reachable and contained in the target monitor in every orientation.
 ResetHidden(d);d.reveal.snap(1);d.hideOff=0;d.Layout(simTime);d.ComputePanelRect();
 auto sensor=d.ScreenPointUV((d.cells[Dock::HitSensor].u0+d.cells[Dock::HitSensor].u1)/2,d.margin+d.barThick/2);
 Advance(d,sensor,0.9);Check(d.panelAlpha.x>0.95f,"sensor hover opens panel");
 Check(d.panelRect.left>=0 && d.panelRect.top>=0 && d.panelRect.right<=d.winW && d.panelRect.bottom<=d.winH,"sensor panel remains inside monitor window");
 auto panel=POINT{d.winX+(LONG)((d.panelRect.left+d.panelRect.right)/2),d.winY+(LONG)((d.panelRect.top+d.panelRect.bottom)/2)};
 Advance(d,panel,1.0);Check(d.panelAlpha.target==1 && d.reveal.target==1,"moving into sensor panel keeps it open");
 Advance(d,Outside(d.mon),2.0);Check(d.reveal.x<0.002f && d.panelAlpha.x<0.01f,"panel and dock close after leaving");
 // Interaction locks and fullscreen suppression are controlled inputs, not OS input injection.
 ResetHidden(d);Advance(d,center,0.8);d.menuOpen=true;Advance(d,Outside(d.mon),1.0);Check(d.reveal.target==1,"open menu prevents hiding");d.menuOpen=false;
 d.dragging=true;Advance(d,Outside(d.mon),1.0);Check(d.reveal.target==1,"drag in progress prevents hiding");d.dragging=false;
 d.pressSlot=0;Advance(d,Outside(d.mon),1.0);Check(d.reveal.target==1,"pressed icon prevents hiding");d.pressSlot=-1;
 Advance(d,Outside(d.mon),1.6);Check(d.reveal.x<0.002f,"releasing interaction lock permits hiding");
 ResetHidden(d);fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;Advance(d,center,1.0);Check(d.reveal.target==0,"fullscreen on this monitor suppresses reveal");
 fgMonitor=(HMONITOR)(uintptr_t)0xD00D;Advance(d,center,1.0);Check(d.reveal.target==1,"fullscreen on another monitor does not suppress this dock");
 ResetHidden(d);fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;fgMaximized=true;Advance(d,center,0.8);Check(d.reveal.target==1,"maximized ordinary window permits reveal");
 ResetHidden(d);fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;fgStyle=WS_CAPTION;Advance(d,center,0.8);Check(d.reveal.target==1,"captioned full-sized window permits reveal");
 ResetHidden(d);fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;fgClass=L"Progman";Advance(d,center,0.8);Check(d.reveal.target==1,"desktop window permits reveal");
 ResetHidden(d);auto corner=EdgePoint(d.mon,g_settings.edge,0.f,0);Advance(d,corner,0.8);
 fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;Advance(d,corner,1.6);
 Check(d.reveal.target==0 && d.reveal.x<0.002f,"fullscreen starting at a corner still permits the dock to hide");
 ResetHidden(d);g_apps.attentionEvents.push_back({1});Advance(d,Outside(d.mon),0.05);g_apps.attentionEvents.clear();
 Check((d.slots[1].bounceCount>0)==d.primary,"attention notification is restricted to primary monitor");
 ResetHidden(d);fgEnabled=true;fgRect=d.mon;fgMonitor=d.monitor;g_apps.attentionEvents.push_back({1});Advance(d,Outside(d.mon),0.05);g_apps.attentionEvents.clear();Check(d.slots[1].bounceCount==0,"fullscreen suppresses app attention bounce");
 // the Windows key: Start (or search, quick settings, notifications) brings a hidden dock up and lets it go again
 {
  auto keepPopup=Dock::isShellPopup;Dock::isShellPopup=[](HWND h){return h==fgHandle;};
  ResetHidden(d);fgEnabled=true;fgMonitor=d.monitor;fgRect={d.mon.left+100,d.mon.top+100,d.mon.left+700,d.mon.top+800};
  Advance(d,Outside(d.mon),0.6);Check(d.reveal.target==1 && d.reveal.x>0.95f,"opening Start shows the hidden dock");
  Advance(d,Outside(d.mon),1.5);Check(d.reveal.target==1,"the dock stays while Start is open");
  fgEnabled=false;Advance(d,Outside(d.mon),1.6);Check(d.reveal.target==0 && d.reveal.x<0.002f,"closing Start puts the dock away again");
  ResetHidden(d);fgEnabled=true;fgMonitor=(HMONITOR)(uintptr_t)0xD00D;fgRect={0,0,600,700};
  Advance(d,Outside(d.mon),0.6);Check(d.reveal.target==0,"Start on another monitor leaves this dock hidden");
  ResetHidden(d);g_settings.showWithStart=false;fgEnabled=true;fgMonitor=d.monitor;fgRect={d.mon.left+100,d.mon.top+100,d.mon.left+700,d.mon.top+800};
  Advance(d,Outside(d.mon),0.6);Check(d.reveal.target==0,"with the option off Start leaves the dock hidden");
  g_settings.showWithStart=true;Dock::isShellPopup=keepPopup;
 }
 ResetHidden(d);
}

// Maximized windows in front (mod.24): only a maximized window without caption, resize frame or WS_POPUP, its
// rectangle and client area exactly the monitor (a browser in F11 or a page's fullscreen video) is fullscreen.
// Maximized windows hang over the monitor by their frame (-11 px at 150 %, -8 px at 100 %).
static void ZoomedTests(Dock& d) {
 const auto center=EdgePoint(d.mon,g_settings.edge,0.5f,0);
 auto front=[&](LONG_PTR style,RECT window,RECT client){ResetHidden(d);fgEnabled=true;fgMonitor=d.monitor;fgMaximized=true;fgStyle=style;fgRect=window;fgClient=client;};
 front(kChromeF11,d.mon,d.mon);Advance(d,center,0.8);
 Check(d.reveal.target==0 && d.FullscreenHere(),"maximized browser in F11 suppresses reveal");
 Check(simTime-d.lastCaptureWanted>0.7,"maximized browser in F11: no capture pre-warmed at the edge");
 for(int frame:{8,11}){
  front(kChromeMaximized,Grown(d.mon,frame),d.mon);Advance(d,center,0.8);
  Check(d.reveal.target==1 && !d.FullscreenHere(),"maximized browser over the monitor by its frame permits reveal");
 }
 front(kChromeF11|WS_THICKFRAME,d.mon,d.mon);Advance(d,center,0.8);
 Check(d.reveal.target==1,"maximized caption-less window with a resize frame permits reveal");
 front(WS_POPUP|WS_VISIBLE|WS_MAXIMIZE,d.mon,d.mon);Advance(d,center,0.8);
 Check(d.reveal.target==1,"maximized WS_POPUP exactly the monitor permits reveal");
 {RECT c=d.mon;c.bottom-=2;front(kChromeF11,d.mon,c);Advance(d,center,0.8);}
 Check(d.reveal.target==1,"maximized frameless window with its client area 2 px short permits reveal");
 {RECT c=d.mon;c.bottom-=1;front(kChromeF11,c,c);Advance(d,center,0.8);}
 Check(d.reveal.target==1,"maximized frameless window 1 px short permits reveal");
 front(kChromeF11,Grown(d.mon,1),d.mon);Advance(d,center,0.8);
 Check(d.reveal.target==1,"maximized frameless window 1 px over the monitor permits reveal");
 front(kChromeF11,d.mon,d.mon);fgInfoFails=true;Advance(d,center,0.8);
 Check(d.reveal.target==1,"maximized frameless window whose GetWindowInfo fails permits reveal");
 // F11, then Esc while the pointer waits at the edge: the dock comes up once the browser is a window again
 front(kChromeF11,d.mon,d.mon);Advance(d,center,0.8);
 fgStyle=kChromeMaximized;fgRect=Grown(d.mon,8);Advance(d,center,0.8);
 Check(d.reveal.target==1 && !d.FullscreenHere(),"leaving F11 permits reveal again");
 ResetHidden(d);
}

// A window that comes to the front over a dock that is up (mod.32). Seen on a test PC, 2026-11-16: Windows Terminal set to
// "always on top", in fullscreen (WS_POPUP, still maximized: no fullscreen app for the dock), the dock up and its
// "show desktop" button pressed twice - the second press brought the terminal back to the front, to the top of the
// topmost windows, over the dock, which stayed up under it while the pointer stayed on its place.
static void FrontTests(Dock& d) {
 HWND own=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"dock_interaction_test front",WS_POPUP,0,0,8,8,nullptr,nullptr,nullptr,nullptr);
 HWND lent=d.hwnd;if(!lent)d.hwnd=own;
 frontDock=d.hwnd;
 const HWND desktop=(HWND)(uintptr_t)0xDE5C,terminal=(HWND)(uintptr_t)0x7E57,other=(HWND)(uintptr_t)0x07E5;
 const POINT center=EdgePoint(d.mon,g_settings.edge,0.5f,0);
 POINT onDock{};   // the middle of the bar, taken once the dock is up (up)
 // the window in front: h, above the dock or not; the terminal as measured (style 95000000, maximized, the monitor)
 auto front=[&](HWND h,bool above){
  fgEnabled=true;fgHandle=h;fgAboveDock=above;fgMonitor=d.monitor;fgRect=d.mon;fgClient=d.mon;
  fgClass=h==desktop?L"Progman":L"CASCADIA_HOSTING_WINDOW_CLASS";fgStyle=(LONG_PTR)0x95000000;fgMaximized=true;
 };
 auto up=[&]{
  ResetHidden(d);fgAboveDock=false;Advance(d,center,0.8);
  onDock=d.ScreenPointUV((d.barU0+d.barU1)*0.5f,d.margin+d.barThick/2);Advance(d,onDock,0.5);
 };
 // the reported case
 up();front(terminal,false);Advance(d,onDock,0.6);
 Check(d.reveal.target==1 && !d.FullscreenHere(),"the terminal in fullscreen (WS_POPUP, maximized) lets the dock up");
 int before=dockRaises,logged=d.frontRaises;
 front(desktop,false);Advance(d,onDock,0.6);
 Check(dockRaises==before,"show desktop: the desktop in front, not over the dock - nothing raised");
 front(terminal,true);Advance(d,onDock,1.f/120.f);
 Check(dockRaises==before+1 && !fgAboveDock,"the terminal back in front over the dock: the dock goes back over it at once");
 Check(d.frontRaises==logged+1,"...counted for dock.log");
 Advance(d,onDock,1.0);
 Check(d.reveal.target==1 && dockRaises==before+1,"...and stays up over it, raised once");
 // the z-order following the activation a little later: the second look
 before=dockRaises;front(other,false);Advance(d,onDock,0.1);
 Check(dockRaises==before,"a new window in front not over the dock: nothing raised");
 fgAboveDock=true;Advance(d,onDock,0.3);
 Check(dockRaises==before+1 && !fgAboveDock,"...over it a moment later: the second look raises the dock");
 // looked at only when the window in front changes (and once more): no SetWindowPos every frame
 before=dockRaises;fgAboveDock=true;Advance(d,onDock,1.0);
 Check(dockRaises==before,"the same window in front: not looked at again");
 // a hidden dock: raised as it comes up (Update), not for the window in front
 ResetHidden(d);before=dockRaises;front(terminal,true);Advance(d,Outside(d.mon),0.6);
 Check(dockRaises==before && d.reveal.target==0,"a hidden dock is not raised for the window in front");
 Advance(d,center,0.8);
 Check(d.reveal.target==1 && dockRaises==before+1,"...it is raised as it comes up");
 // only a window over the whole bar: a program's pop-up from a tray icon or a picture-in-picture video over part
 // of it stays over the dock, as before (review of mod.32)
 {
  up();
  const auto b=d.RectUV(d.barU0,d.barU1,d.margin,d.margin+d.barThick);   // where the bar sits when up, as KeepInFront takes it
  const RECT bar={d.winX+(LONG)std::floor(b.left),d.winY+(LONG)std::floor(b.top),d.winX+(LONG)std::ceil(b.right),d.winY+(LONG)std::ceil(b.bottom)};
  const LONG w=bar.right-bar.left,h=bar.bottom-bar.top;
  auto over=[&](RECT r){front(other,false);Advance(d,onDock,0.6);before=dockRaises;front(terminal,true);fgMaximized=false;fgStyle=WS_POPUP|WS_VISIBLE;fgRect=r;Advance(d,onDock,0.6);return dockRaises-before;};
  Check(over({bar.left+w/4,bar.top+h/4,bar.right-w/4,bar.bottom-h/4})==0 && d.reveal.target==1,"a smaller window over part of the bar (a tray pop-up, a video): not gone over");
  Check(over({bar.left+1,bar.top,bar.right,bar.bottom})==0,"a window 1 px short of the bar: not gone over");
  Check(over(bar)==1,"a window exactly over the bar: gone over");
 }
 // the shell's pop-ups (the real program rule left in place), Explorer's windows: not gone over
 {
  auto keepPopup=Dock::isShellPopup;Dock::isShellPopup=[](HWND h){return h==(HWND)(uintptr_t)0x7E57;};
  up();front(other,false);Advance(d,onDock,0.6);before=dockRaises;front(terminal,true);Advance(d,onDock,0.6);
  Check(dockRaises==before && d.reveal.target==1,"a shell pop-up in front over the dock (Start, search): not gone over");
  Dock::isShellPopup=keepPopup;
  auto keep=Dock::raiseOver;Dock::raiseOver=[](HWND){return false;};
  up();front(other,false);Advance(d,onDock,0.6);before=dockRaises;front(terminal,true);Advance(d,onDock,0.6);
  Check(dockRaises==before && d.reveal.target==1,"a window raiseOver turns down (Explorer's: Alt+Tab, Task view): not gone over");
  Dock::raiseOver=keep;
 }
 Check(!Dock::raiseOver(own),"the dock's own windows are not gone over");
 if(HWND shell=GetShellWindow())Check(!Dock::raiseOver(shell),"Explorer's windows (the desktop's here) are not gone over");
 Check(Dock::raiseOver(terminal),"another program's window is gone over");
 // a fullscreen app in front: the dock goes away, nothing raised
 up();front(other,false);Advance(d,onDock,0.6);before=dockRaises;
 front(terminal,true);fgMaximized=false;fgStyle=WS_POPUP|WS_VISIBLE;Advance(d,onDock,1.0);
 Check(dockRaises==before && d.reveal.target==0,"a fullscreen app coming to the front: the dock goes away, not raised");
 fgHandle=(HWND)(uintptr_t)0xF00DFACE;fgAboveDock=false;frontDock=nullptr;
 ResetHidden(d);
 if(!lent){d.hwnd=nullptr;d.cloaked=FALSE;d.curRegion={-1,-1,-1,-1};}
 if(own)DestroyWindow(own);
}

static void SweepTests(Dock& d) {
 const POINT center=EdgePoint(d.mon,g_settings.edge,0.5f,0);
 ResetHidden(d);Advance(d,center,1.5);
 // reference with the pointer over the dock itself (an offset dock is not under the screen centre)
 Advance(d,d.ScreenPointUV((d.barU0+d.barU1)*0.5f,d.margin+d.barThick/2),1.0);
 const float from=d.slots.front().u,to=d.slots.back().u;
 const float referenceCenter=(d.barU0+d.barU1)*0.5f;
 const float referenceLength=d.barU1-d.barU0;
 float maxCenterError=0,maxLengthError=0;
 bool remainedVisible=true,sizesBounded=true,ordered=true;
 // Fixed physical points from the displayed first/last icons, not points that
 // follow a moving dock. Several return trips expose frame-to-frame feedback.
 for(int lap=0;lap<3;++lap)for(int direction=0;direction<2;++direction) {
  for(int frame=0;frame<=120;++frame) {
   float u=lerpf(direction?to:from,direction?from:to,frame/120.f);
   auto p=d.ScreenPointUV(u,d.margin+d.barThick/2);
   simTime+=1.f/120.f;d.Update(simTime,1.f/120.f,p);
   maxCenterError=std::max(maxCenterError,std::fabs((d.barU0+d.barU1)*0.5f-referenceCenter));
   maxLengthError=std::max(maxLengthError,std::fabs(d.barU1-d.barU0-referenceLength));
   remainedVisible &= d.reveal.target>0.5f;
   for(size_t i=0;i<d.slots.size();++i){
    const auto& s=d.slots[i];sizesBounded &= std::isfinite(s.size) && s.size>=d.base-0.01f && s.size<=d.base*d.mag+0.01f;
    if(i)ordered &= d.slots[i-1].u0+d.slots[i-1].size*d.slots[i-1].weight<=s.u0+0.01f;
   }
  }
 }
 Check(maxCenterError<=0.1f,"full-length return trips preserve dock center");
 Check(maxLengthError<=0.1f,"full-length return trips preserve enlarged dock length");
 Check(remainedVisible,"full-length return trips never hide the dock");
 Check(sizesBounded,"sweeps keep every icon within configured magnification");
 Check(ordered,"sweeps never overlap or reorder icons");
 if(maxCenterError>0.1f || maxLengthError>0.1f)report<<"SWEEP_DETAIL\t"<<caseLabel<<"\tcenter="<<maxCenterError<<" length="<<maxLengthError<<"\n";
 Advance(d,Outside(d.mon),2);
 Check(d.reveal.x<0.002f && IsRectEmpty(&d.curRegion),"dock hides normally after pointer leaves a sweep");
 Advance(d,center,1.5);
 Check(std::fabs((d.barU0+d.barU1)*0.5f-referenceCenter)<=0.1f,"reopening after a sweep returns to the same center");
 ResetHidden(d);
}

// Leaving the magnified apps sideways (mod.28): the icons shrink back, and the end of the dock on that side - the
// tray's, or the start of the bar - stays where it was while they do; only the other end comes in. Back on the
// apps the dock grows out to its centred place; put away (or, staying up, once the pointer has gone) it is centred.
static void SideExitTests(Dock& d) {
 if(d.mag<1.05f||d.slots.size()<2)return;
 const POINT center=EdgePoint(d.mon,g_settings.edge,0.5f,0);
 const float v=d.margin+d.barThick/2;
 ResetHidden(d);Advance(d,center,1.5);
 Advance(d,d.ScreenPointUV((d.slots.front().u+d.slots.back().u)*0.5f,v),1.0);
 const float centred=(d.barU0+d.barU1)*0.5f;   // magnified over the apps: the dock's place at any size
 for(int side=1;side>=0;--side){   // 1: onto the clock, 0: just past the start of the bar
  ResetHidden(d);Advance(d,center,1.5);
  const size_t end=side?d.slots.size()-1:0;
  POINT icon{};for(int attempt=0;attempt<6;++attempt){icon=d.ScreenPointUV(d.slots[end].u,v);Advance(d,icon,0.3);}
  if(!side && d.barU0<8*d.scale)continue;   // no screen before the bar for the pointer
  Check(d.magAmount.x>0.98f && d.slots[end].size>d.base*(1+(d.mag-1)*0.5f),"side exit: the end icon is enlarged under the pointer");
  const POINT beside=d.ScreenPointUV(side?d.cells[Dock::HitClock].Mid():d.barU0-6*d.scale,v);
  const float held=side?d.barU1:d.barU0,other=side?d.barU0:d.barU1,clock=d.cells[Dock::HitClock].u0,sep=d.sepU;
  float drift=0;bool onClock=true;
  auto track=[&]{
   drift=std::max(drift,std::fabs((side?d.barU1:d.barU0)-held));
   if(side)drift=std::max({drift,std::fabs(d.cells[Dock::HitClock].u0-clock),std::fabs(d.sepU-sep)});
  };
  for(int f=1;f<=24;++f){POINT p{icon.x+(beside.x-icon.x)*f/24,icon.y+(beside.y-icon.y)*f/24};Advance(d,p,1.f/120);track();}
  for(int f=0;f<96;++f){Advance(d,beside,1.f/120);track();if(side)onClock&=d.hoverKind==Dock::HitClock;}
  bool rest=true;for(auto& s:d.slots)rest&=s.size<=d.base+0.01f;
  Check(d.magAmount.target==0 && rest,"side exit: beside the apps every icon is back to its size");
  Check(drift<=0.1f,"side exit: the end of the dock beside the pointer stays put while the apps shrink");
  if(drift>0.1f)report<<"SIDE_EXIT_DETAIL\t"<<caseLabel<<"\tside="<<side<<" drift="<<drift<<"\n";
  if(side)Check(onClock,"side exit: the clock stays under the pointer on every frame");
  Check(d.reveal.target==1 && d.pinSide==(side?1:-1),"side exit: the dock stays up with that end held");
  Check((side?d.barU0-other:other-d.barU1)>=d.base*(d.mag-1)*0.9f,"side exit: the other end comes in instead");
  icon=d.ScreenPointUV(d.slots[end].u,v);drift=0;
  for(int f=0;f<96;++f){Advance(d,icon,1.f/120);track();}
  Check(d.magAmount.x>0.98f && d.slots[end].size>d.base*(1+(d.mag-1)*0.5f) && drift<=0.1f,"side exit: back on the icon it grows again without moving that end");
  Check(d.pinSide==0 && std::fabs((d.barU0+d.barU1)*0.5f-centred)<=0.1f,"side exit: fully grown the dock is at its centred place, nothing held");
  for(int lap=0;lap<3;++lap){Advance(d,beside,0.25);Advance(d,icon,0.25);}
  Advance(d,beside,0.8);
  Check(std::fabs((side?d.barU1:d.barU0)-held)<=0.1f && d.slots[end].size<=d.base+0.01f,"side exit: quick crossings leave that end where it was");
  Advance(d,Outside(d.mon),2);
  if(d.AlwaysShown()){
   Check(d.pinSide==0 && std::fabs((d.barU0+d.barU1)*0.5f-centred)<=0.1f,"side exit: the pointer gone, a dock that stays up eases back to its place");
   Check(d.pinBlend.settled() && d.pinBlend.x==0 && d.magAmount.settled(),"side exit: ...and nothing of it is left animating");
  }else{
   Check(d.reveal.x<0.002f && d.pinSide==0 && d.pinBlend.x==0,"side exit: put away, the end is let go");
   Advance(d,center,1.5);
   Check(std::fabs((d.barU0+d.barU1)*0.5f-centred)<=0.1f,"side exit: shown again, the dock is centred as before");
  }
 }
 ResetHidden(d);
}

// The same in Layout alone, each end, also with FluentFlyout's widget slot open before the start button (the
// matrix has no FluentFlyout: the slot is opened by hand here).
static void PinnedLayoutTests(Dock& d) {
 if(d.mag<1.05f||d.slots.size()<7)return;
 ResetHidden(d);
 const float step=12*d.scale,room=4*d.scale+1;
 for(float lead:{0.f,std::ceil(200*d.scale/step)*step+d.gap})for(int side:{-1,1}){
  d.widgetWidth.snap(lead);d.pinSide=0;d.pinBlend.snap(0);
  d.magAmount.snap(0);d.Layout(0);
  const float c0=d.barU0,c1=d.barU1;const size_t k=d.slots.size()/2;const float rest=d.slots[k].u;   // at rest, centred
  d.magAmount.snap(1);d.hoverU=side<0?d.slots.front().u:d.slots.back().u;d.Layout(0);
  if(d.barU0<room||d.barU1>d.len-room)continue;   // no room to magnify with the slot open: the screen's end decides there
  const float u0=d.barU0,u1=d.barU1,w0=d.widgetU0,w1=d.widgetU1,a0=d.appsU0,sep=d.sepU,clock=d.cells[Dock::HitClock].u0;
  d.pinSide=side;d.pinU=side>0?u1:u0;d.pinBlend.snap(1);
  float worst=0;
  for(int i=20;i>=0;--i){
   d.magAmount.snap(i/20.f);d.Layout(0);
   worst=std::max(worst,side<0?std::max({std::fabs(d.barU0-u0),std::fabs(d.widgetU0-w0),std::fabs(d.widgetU1-w1),std::fabs(d.appsU0-a0)})
                              :std::max({std::fabs(d.barU1-u1),std::fabs(d.sepU-sep),std::fabs(d.cells[Dock::HitClock].u0-clock)}));
  }
  Check(worst<=0.05f,side<0?"start held: the bar's start, FluentFlyout's slot and the first app stay put at every magnification"
                           :"end held: the separator, the clock and the bar's end stay put at every magnification");
  bool atRest=true;for(auto& s:d.slots)atRest&=std::fabs(s.size-d.base)<0.01f;
  Check(atRest && std::fabs(side<0?d.barU1-(u0+c1-c0):d.barU0-(u1-(c1-c0)))<=0.05f,"held, not magnified: every icon at its size and the dock as long as at rest");
  d.pinBlend.snap(0.5f);d.Layout(0);
  Check(std::fabs(d.barU0-(c0+(side<0?u0:u1-(c1-c0)))*0.5f)<=0.05f,"easing back: halfway between held and centred");
  d.pinBlend.snap(1);d.magAmount.snap(0.5f);d.Layout(0);   // pinOff: how far off centre this magnification is held
  d.hoverU=rest+d.pinOff;d.Layout(0);
  Check(std::fabs(d.pinOff)>d.base*(d.mag-1)*0.2f && d.slots[k].size>d.slots[k-1].size && std::fabs(d.slots[k-1].size-d.slots[k+1].size)<=d.base*(d.mag-1)*0.1f,
        "held off centre: the icon under the pointer is the one enlarged, its neighbours alike");
  d.pinSide=0;d.magAmount.snap(0.4f);d.Layout(0);
  d.pinSide=side;d.pinU=side>0?d.barU1:d.barU0;d.pinBlend.snap(1);d.magAmount.snap(1);d.Layout(0);
  Check(std::fabs(d.barU0-u0)<=0.05f && std::fabs(d.barU1-u1)<=0.05f,"growing again past an end held at part size: the centred dock, not pushed past its place");
  d.pinSide=0;d.pinBlend.snap(0);d.magAmount.snap(0);d.Layout(0);
  Check(std::fabs(d.barU0-c0)<=0.05f && std::fabs(d.barU1-c1)<=0.05f,"let go: the dock is centred as at rest");
 }
 d.widgetWidth.snap(0);d.pinSide=0;d.pinBlend.snap(0);d.magAmount.snap(0);d.hoverU=0;d.Layout(0);
}

// The dock gets shorter while an end is held (mod.28 review): an app closes its last window with the pointer on the
// clock, or FluentFlyout's slot shuts under a pointer on the widget (opened by hand as above, before every frame:
// Update finds no FluentFlyout and shuts it - which is how it shuts for good once it is no longer opened). Back on
// the apps and fully grown, the shorter dock does not reach its centred place by growing; it stayed half the lost
// length off it for as long as the pointer was on the apps. It eases back to where a dock of that length that never
// had an end held is. On docks of its own: the case's dock keeps its apps and slots.
static void SideExitShorterTests(const MonitorCase& monitor,int index) {
 const auto apps=g_apps.items;
 for(int side=1;side>=0;--side){   // 1: the tray end, an app closes; 0: the start of the bar, FluentFlyout's slot shuts
  if(!side && IsVertical(g_settings.edge))continue;   // FluentFlyout's widget: docks at the top and bottom only
  Dock d(monitor.handle,index,monitor.primary);InitializeLayout(d);
  if(d.mag<1.05f||d.slots.size()<7)return;
  const POINT center=EdgePoint(d.mon,g_settings.edge,0.5f,0);
  const float v=d.margin+d.barThick/2,step=12*d.scale,lead=side?0:std::ceil(200*d.scale/step)*step+d.gap;
  auto run=[&](POINT p,int frames){for(int f=0;f<frames;++f){if(lead>0)d.widgetWidth.snap(lead);Advance(d,p,1.f/120);}};
  run(center,180);
  const size_t end=side?d.slots.size()-1:0;
  POINT icon{};for(int attempt=0;attempt<6;++attempt){icon=d.ScreenPointUV(d.slots[end].u,v);run(icon,36);}
  const POINT beside=d.ScreenPointUV(side?d.cells[Dock::HitClock].Mid():(d.widgetU0+d.widgetU1)*0.5f,v);
  for(int f=1;f<=24;++f){POINT p{icon.x+(beside.x-icon.x)*f/24,icon.y+(beside.y-icon.y)*f/24};run(p,1);}
  run(beside,96);
  const bool held=d.pinSide==(side?1:-1) && d.magAmount.target==0;
  const float trayEnd=d.barU1;float drift=0;
  if(side){g_apps.items.erase(g_apps.items.begin()+g_apps.items.size()/2);++g_apps.structureVersion;}
  for(int f=0;f<240;++f){Advance(d,beside,1.f/120);if(side)drift=std::max(drift,std::fabs(d.barU1-trayEnd));}   // (the slot, opened no more, shuts)
  if(side)Check(held && d.pinSide==1 && drift<=0.1f,"side exit, shorter: an app closes while the tray end is held - that end stays where it is");
  else Check(held && d.widgetWidth.x==0,"side exit, shorter: FluentFlyout's slot shuts while the start of the dock is held");
  const POINT mid=d.ScreenPointUV(d.slots[d.slots.size()/2].u,v);
  Advance(d,mid,2);
  Dock fresh(monitor.handle,index,monitor.primary);InitializeLayout(fresh);   // a dock of that length, no end of it ever held
  Advance(fresh,center,1.5);
  bool never=true;for(int f=0;f<240;++f){Advance(fresh,mid,1.f/120);never&=fresh.pinSide==0;}
  Check(d.magAmount.target==1 && d.magAmount.x>0.98f && never && fresh.magAmount.x>0.98f,"side exit, shorter: back on the apps the dock is fully grown");
  Check(d.pinSide==0 && std::fabs(d.barU0-fresh.barU0)<=0.1f && std::fabs(d.barU1-fresh.barU1)<=0.1f,
        "side exit, shorter: ...and where a dock of that length never held is, nothing held any more");
  if(d.pinSide!=0||std::fabs(d.barU0-fresh.barU0)>0.1f||std::fabs(d.barU1-fresh.barU1)>0.1f||(side&&drift>0.1f))
   report<<"SIDE_EXIT_SHORTER\t"<<caseLabel<<"\tside="<<side<<" pinSide="<<d.pinSide<<" start off="<<d.barU0-fresh.barU0<<" end off="<<d.barU1-fresh.barU1<<" drift="<<drift<<"\n";
  if(side){g_apps.items=apps;++g_apps.structureVersion;}
 }
}

static void ReachabilityTests(Dock& d) {
 for(size_t index=0;index<d.slots.size();++index){
  ResetHidden(d);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),1.5);
  bool reachable=false;POINT p{};
  for(int attempt=0;attempt<6 && !reachable;++attempt){
   p=d.ScreenPointUV(d.slots[index].u,d.margin+d.barThick/2);
   Advance(d,p,0.3);float u,v;d.ScreenToUV(p,u,v);auto hit=d.HitTestUV(u,v);
   reachable=hit.kind==Dock::HitSlot && hit.slot==(int)index && d.reveal.target>0.5f;
  }
  Check(reachable,"each icon can be reached after magnification settles");
  if(reachable){
   Advance(d,p,0.6);float u,v;d.ScreenToUV(p,u,v);auto hit=d.HitTestUV(u,v);
   Check(hit.kind==Dock::HitSlot && hit.slot==(int)index,"reached icon remains the stable input target");
  }
 }
 ResetHidden(d);
}

static void OptionsTests(Dock& d) {
 ResetHidden(d);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),1.5);
 AssertHitGeometry(d);
 auto clock=d.RectUV(d.cells[Dock::HitClock].u0,d.cells[Dock::HitClock].u1,d.margin,d.margin+d.barThick);
 bool vertical=IsVertical(g_settings.edge);
 auto time=d.TextSize(d.ClockFont(),vertical?g_sys.timeShort:g_sys.timeText);
 auto date=d.TextSize(d.ClockFont(true),g_sys.dateText);
 Check(time.width<=clock.right-clock.left+0.1f,"clock text width fits without clipping");
 Check((vertical?time.height:time.height+date.height)<=clock.bottom-clock.top+0.1f,"clock text height fits without clipping");
 if(!vertical)Check(date.width<=clock.right-clock.left+0.1f,"date text width fits without clipping");
 Check(d.ClockFont()->GetFontSize()>g_gfx.fmtClock->GetFontSize(),"new normal and large clocks exceed former clock font size");
 Check(std::fabs(g_gfx.fmtClock->GetFontSize()-13*d.scale)<0.1f,"sensor font is independent of clock setting");
 // With no pressure to fit, clock size and button visibility must not enlarge icons.
 auto normalMetrics=d.MeasureMetrics(1);
 Check(d.base<=normalMetrics.icon,"clock space never increases app icon size");
 const int before=desktopActions;
 d.Click(Dock::ButtonHit(ButtonId::Desktop)); // stubbed Shell action: never minimizes real windows
 Check(desktopActions-before==1,"desktop button routes exactly one Shell action");
 if(HasButton(ButtonId::Desktop)){
  auto br=d.ButtonRect(ButtonId::Desktop);POINT p={d.winX+(LONG)((br.left+br.right)*0.5f),d.winY+(LONG)((br.top+br.bottom)*0.5f)};
  Advance(d,p,0.8);
  Check(d.hoverKind==Dock::HitButton && d.hoverButton==ButtonId::Desktop && d.reveal.target==1,"desktop button stays reachable with dock visible");
  Check(d.panelAlpha.target==0,"desktop hover does not open sensor panel");
 }
 auto sr=d.ButtonRect(ButtonId::Settings);POINT settingsPoint={d.winX+(LONG)((sr.left+sr.right)*0.5f),d.winY+(LONG)((sr.top+sr.bottom)*0.5f)};
 Advance(d,settingsPoint,0.8);
 Check(d.hoverButton==ButtonId::Settings && d.reveal.target==1,"gear remains reachable after leaving magnified apps");
 Check(d.panelAlpha.target==0,"settings hover does not open the sensor panel");
 int windowsBefore=settingsWindows,menuBefore=settingsMenus,desktopBefore=desktopActions;
 d.Click(Dock::ButtonHit(ButtonId::Settings)); // popup display is intercepted; no real menu or focus changes
 Check(settingsWindows==windowsBefore+1 && settingsMenus==menuBefore && desktopActions==desktopBefore,"gear click opens the settings window, not a menu or the desktop action");
 for(auto id:{ButtonId::Network,ButtonId::Volume}){
  auto r=d.ButtonRect(id);
  POINT p={d.winX+(LONG)((r.left+r.right)*0.5f),d.winY+(LONG)((r.top+r.bottom)*0.5f)};
  Advance(d,p,0.8);
  Check(d.hoverButton==id && d.reveal.target==1,"connectivity hover selects its own tooltip and keeps dock visible");
  Check(!d.tooltipText.empty() && d.tooltipAlpha.target==1,"connectivity tooltip remains available in paired row");
 }
 ResetHidden(d);
}

static void DragSpacingTests(Dock& d) {
 ResetHidden(d);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),1.5);
 auto moved=d.slots[1].app;const size_t count=g_apps.items.size();
 d.pressSlot=1;d.dragging=true;d.slots[1].shrink.target=0;
 // Controlled cursor inputs go through production drop-index/layout logic.
 auto target=d.ScreenPointUV(d.sepU,d.margin+d.barThick/2);
 Advance(d,target,0.7);
 Check(!d.dropRemove && d.dropIndex==(int)count-1,"drag reaches final insertion position at every spacing");
 d.EndDrag(true);Advance(d,target,1.0);
 Check(g_apps.items.size()==count && g_apps.items.back()==moved && moved->pinned,"drag changes order without losing or unpinning an app");
 auto cancelFirst=g_apps.items.front();
 d.pressSlot=1;d.dragging=true;d.slots[1].shrink.target=0;
 Advance(d,d.ScreenPointUV(d.sepU,d.margin+d.barThick/2),0.4);
 d.EndDrag(false);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),1.0);
 Check(g_apps.items.front()==cancelFirst && g_apps.items.back()==moved,"cancelled drag preserves app order");
 { // unpinning: past the line where the dragged icon is wholly cut off by the window's region (dock.h UnpinLine), not T + 20
  const float line=d.UnpinLine(),u=d.slots[2].u;
  Check(line>=d.margin+d.pad+d.base*d.mag+10*d.scale-0.01f && line>d.margin+d.barThick && line<d.T,
        "unpin line: above a magnified icon's top, under the window's top");
  d.pressSlot=1;d.dragging=true;d.slots[1].shrink.target=0;
  Advance(d,d.ScreenPointUV(u,line-3*d.scale),0.6);
  Check(d.dragging && !d.dropRemove,"a drag just under the unpin line only moves the app");
  Advance(d,d.ScreenPointUV(u,line+3*d.scale),0.6);
  const float vb=std::clamp(d.cursorV-d.base/2,-d.base,d.T-d.base);
  const D2D1_RECT_F f=d.RectUV(d.cursorU-d.base/2,d.cursorU+d.base/2,vb,vb+d.base);
  const RECT icon={(LONG)std::floor(f.left),(LONG)std::floor(f.top),(LONG)std::ceil(f.right),(LONG)std::ceil(f.bottom)};
  RECT both{};
  Check(d.dropRemove && !IntersectRect(&both,&icon,&d.curRegion),"a drag past the unpin line unpins - the dragged icon is out of sight there");
  d.EndDrag(false);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),1.0);
  Check(moved->pinned && g_apps.items.back()==moved,"a cancelled drag past the line unpins nothing");
 }
 AssertHitGeometry(d);ResetHidden(d);
}

static void RenderFixture(const MonitorCase& monitor,int edge,int index,int variant=0) {
 g_settings=Settings{};g_settings.edge=(Edge)edge;Apps(12);fgEnabled=false;
 if(variant==1 || variant==2){g_settings.clockSize=1;g_settings.iconSpacing=2;}
 if(variant==2)SetButton(ButtonId::Desktop,false);
 if(variant==3)g_settings.iconSpacing=0;
 g_sys.timeText=variant?L"오후 12:59":L"21:30";g_sys.timeShort=variant?L"12:59":L"21:30";
 g_sys.dateText=variant?L"12/31 수요일":L"2026-10-07";
 {  // a laptop's battery in two variants: charging, and low on battery
  battery::State bat;
  if(variant==1||variant==2){bat.present=true;bat.percent=variant==1?64:12;bat.charging=bat.plugged=variant==1;}
  g_sys.SetBattery(bat);
 }
 g_sensors.live=true;g_sensors.d={};g_sensors.d.flags=LD_HAS_GPU|LD_HAS_GPU_LOAD|LD_HAS_SSD_TEMP;
 g_sensors.d.cpuLoad=46;g_sensors.d.gpuLoad=72;g_sensors.d.gpuTemp=65;g_sensors.d.gpuMemUsed=5.5f;g_sensors.d.gpuMemTotal=16;
 g_sensors.d.ramUsed=18.5f;g_sensors.d.ramTotal=32;g_sensors.d.ramLoad=58;g_sensors.d.ssdTemp=43;
 wcscpy_s(g_sensors.d.cpuName,L"Sample CPU");wcscpy_s(g_sensors.d.gpuName,L"Sample GPU");
 {  // the panel's drives, memory and VRAM: five drives (one without a temperature, one hot), two modules (one unreported)
  PanelData p;
  const wchar_t* models[]={L"WD SN850X",L"Samsung 870 EVO",L"Fanxiang S880",L"Lexar ARES",L"KIOXIA EXCERIA PLUS G4"};
  const float temps[]={43,29,NAN,39,72};
  for(int i=0;i<5;++i){sensorlogic::DriveRow r;r.letter=wchar_t(L'C'+i);r.disk=i;r.system=i==0;r.model=models[i];r.temp=temps[i];p.drives.push_back(r);}
  sensorlogic::MemoryModule a,b;a.bank=L"P0 CHANNEL A";a.device=L"DIMM 1";a.installed=true;b.bank=L"P0 CHANNEL B";b.device=L"DIMM 1";b.installed=true;
  sensorlogic::DimmTemp t;t.slot=1;t.locator=L"P0 CHANNEL A/DIMM 1";t.celsius=32.8f;
  p.dimms=sensorlogic::BuildDimmRows({a,b},{t});
  p.drivesWithTemp=sensorlogic::CountWithTemp(p.drives);p.modules=2;p.dimmsWithTemp=sensorlogic::CountWithTemp(p.dimms);p.sampled=true;
  // the GPU's memory as the cell's sample has it (5.5 of 16 GB); its temperature known, unknown (the hint) or warm by variant
  p.vram.present=true;p.vram.usedGB=5.5f;p.vram.totalGB=16;p.vram.load=34.4f;p.vram.temp=variant==1?NAN:variant==2?93.0f:61.0f;
  g_sensors.panel=p;
 }
 Dock d(monitor.handle,index,monitor.primary);
 if(!d.Create()){Check(false,"GPU fixture created");return;}
 d.nextCaptureTry=std::numeric_limits<double>::max();
 // An artificial background exercises the existing glass shader without capturing user content.
 std::vector<uint32_t> pixels(d.winW*d.winH);
 for(int y=0;y<d.winH;++y)for(int x=0;x<d.winW;++x){unsigned grid=((x/80+y/80)&1)?28:0;unsigned r=45+grid+(90*x/d.winW),g=65+grid+(80*y/d.winH),b=115+grid;pixels[y*d.winW+x]=0xff000000|(r<<16)|(g<<8)|b;}
 g_gfx.ctx->UpdateSubresource(d.backdrop.tex.Get(),0,nullptr,pixels.data(),d.winW*4,0);
 d.capture.gotFirstFrame=true;d.backdropDirty=true;
 Advance(d,EdgePoint(d.mon,(Edge)edge,0.5f,0),1.0);
 d.snapshotPending=true;d.dirty=true;d.Render();
 Check(!g_gfx.DeviceLost(),"GPU remains available after normal dock rendering");
 d.index=index+1000;Dock::debugForcePanel=true;d.reveal.snap(1);
 Advance(d,d.ScreenPointUV((d.cells[Dock::HitSensor].u0+d.cells[Dock::HitSensor].u1)/2,d.margin+d.barThick/2),1.0);
 d.snapshotPending=true;d.dirty=true;d.Render();
 Check(!g_gfx.DeviceLost(),"GPU remains available after sensor panel rendering");
 Dock::debugForcePanel=false;
 for(int end=0;end<2;++end){
  ResetHidden(d);Advance(d,EdgePoint(d.mon,(Edge)edge,0.5f,0),1.5);
  const auto& slot=end?d.slots.back():d.slots.front();
  Advance(d,d.ScreenPointUV(slot.u,d.margin+d.barThick/2),1.0);
  d.index=index+(end?3000:2000);d.snapshotPending=true;d.dirty=true;d.Render();
  Check(!g_gfx.DeviceLost(),"GPU remains available after end-icon magnification rendering");
 }
 g_sys.SetBattery({});
}

static void RenderOptions(int actualCount) {
 for(int variant=0;variant<4;++variant)for(int i=0;i<actualCount;++i)for(int edge=0;edge<4;++edge){
  caseLabel=std::string("render ")+monitors[i].name+" edge="+std::to_string(edge)+" variant="+std::to_string(variant);
  RenderFixture(monitors[i],edge,100+variant*10000+i*10+edge,variant);
 }
}

static void TraceReproductions(int actualCount) {
 std::ofstream trace(AppDataDir()+L"\\reproduction-trace.csv");
 trace<<"monitor,edge,scenario,seconds,cursor_u,cursor_v,bar_start,bar_end,reveal,reveal_target,magnification,first_icon_center,hit_kind,hit_slot\n";
 for(int mi=0;mi<actualCount;++mi)for(int edge=0;edge<4;++edge)for(int scenario=0;scenario<2;++scenario){
  const auto& monitor=monitors[mi];g_settings=Settings{};g_settings.edge=(Edge)edge;
  if(scenario==1){g_settings.iconSize=54;g_settings.magnification=2.1f;}
  Apps(12);Dock d(monitor.handle,0,monitor.primary);InitializeLayout(d);
  POINT cursor;
  if(scenario==0)cursor=EdgePoint(d.mon,(Edge)edge,0.f,0);
  else {Advance(d,EdgePoint(d.mon,(Edge)edge,0.5f,0),0.8);cursor=d.ScreenPointUV(d.slots[0].u,d.margin+d.barThick/2);}
  float initialU,initialV;d.ScreenToUV(cursor,initialU,initialV);auto initialHit=d.HitTestUV(initialU,initialV);
  int flips=0;float previous=d.reveal.target;
  for(int frame=0;frame<=360;++frame){
   if(frame)Advance(d,cursor,1.f/120.f);
   float u,v;d.ScreenToUV(cursor,u,v);auto hit=d.HitTestUV(u,v);
   if(d.reveal.target!=previous){++flips;previous=d.reveal.target;}
   trace<<monitor.name<<","<<edge<<","<<(scenario?"first-icon-large-strong":"stationary-corner")<<","<<frame/120.0<<","<<u<<","<<v<<","<<d.barU0<<","<<d.barU1<<","<<d.reveal.x<<","<<d.reveal.target<<","<<d.magAmount.x<<","<<d.slots[0].u<<","<<(int)hit.kind<<","<<hit.slot<<"\n";
  }
  printf("TRACE %s edge=%d scenario=%s initial_hit_kind=%d initial_slot=%d visibility_flips=%d final_target=%.0f\n",monitor.name,edge,scenario?"first-icon-large-strong":"stationary-corner",(int)initialHit.kind,initialHit.slot,flips,d.reveal.target);
 }
}

static void PreviewCouplingTests(int actualCount) {
 WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=L"LiquidDockPreviewCouplingSource";wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);
 for(int monitorIndex=0;monitorIndex<actualCount;++monitorIndex)for(int edge=0;edge<4;++edge){
  const auto& monitor=monitors[monitorIndex];++cases;caseFailures=0;caseLabel="preview coupling monitor="+std::to_string(monitorIndex)+" edge="+std::to_string(edge);
  HWND source=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Preview coupling fixture",WS_OVERLAPPEDWINDOW,monitor.rect.left+200,monitor.rect.top+200,400,250,nullptr,nullptr,wc.hInstance,nullptr);
  ShowWindow(source,SW_SHOWNOACTIVATE);
  g_settings=Settings{};g_settings.edge=(Edge)edge;Apps(12);
  {
   Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);d.slots[1].app->windows={source};
   Advance(d,EdgePoint(d.mon,g_settings.edge,.5f,0),1.5);
   POINT icon{};for(int attempt=0;attempt<6;++attempt){icon=d.ScreenPointUV(d.slots[1].u,d.margin+d.barThick/2);Advance(d,icon,.6);}
   Check(d.windowPreview.Open(),"hover opens preview through production Dock update");
   if(d.windowPreview.Open()){
    HWND popup=FindWindowW(L"LiquidDockWindowPreview",nullptr);RECT r{};GetWindowRect(popup,&r);
    POINT card{(r.left+r.right)/2,(r.top+r.bottom)/2};
    float center=(d.barU0+d.barU1)*.5f,length=d.barU1-d.barU0;
    for(int step=0;step<=90;++step){POINT cursor{icon.x+(card.x-icon.x)*step/90,icon.y+(card.y-icon.y)*step/90};Advance(d,cursor,1.f/120);}
    Advance(d,card,.8);
    Check(d.windowPreview.Open(),"crossing from icon to popup keeps preview open");
    Check(d.reveal.target==1,"preview hover keeps dock revealed");
    Check(std::fabs((d.barU0+d.barU1)*.5f-center)<.1f&&std::fabs(d.barU1-d.barU0-length)<.1f,"preview hover preserves dock center and expanded length");
    d.menuOpen=true;Advance(d,card,.1);Check(!d.windowPreview.Open(),"menu suppresses preview");d.menuOpen=false;
    Advance(d,Outside(d.mon),1);Check(d.reveal.target==0,"leaving preview hides dock normally");
    Advance(d,EdgePoint(d.mon,g_settings.edge,.5f,0),1.5);
    icon=d.ScreenPointUV(d.slots[1].u,d.margin+d.barThick/2);Advance(d,icon,1);
    fgEnabled=true;fgMonitor=monitor.handle;fgRect=monitor.rect;fgStyle=WS_POPUP;fgClass=L"MatrixFullscreen";
    Advance(d,icon,.8);Check(!d.windowPreview.Open()&&d.reveal.target==0,"fullscreen closes preview and hides dock");fgEnabled=false;
   }
  }
  DestroyWindow(source);printf("PREVIEW_COUPLING monitor=%d edge=%d failures=%d\n",monitorIndex,edge,caseFailures);fflush(stdout);
 }
}

// ---- units: the button list, the recent-files scan and the trash drop (no real files are deleted)
static void UnitTests() {
 ++cases;caseFailures=0;caseLabel="units";
 Check(FormatButtons(ParseButtons("trash, awake,trash,network,settings,bogus,,quiet"))=="trash,awake,quiet","button list drops duplicates, fixed buttons and unknown keys, keeps order");
 Check(ParseButtons("").empty() && FormatButtons({}).empty(),"an empty button list stays empty");
 g_settings=Settings{};g_settings.trayButtons="theme";
 SetButton(ButtonId::Trash,true);SetButton(ButtonId::Trash,true);
 Check(g_settings.trayButtons=="theme,trash","adding a button appends it once");
 SetButton(ButtonId::Theme,false);SetButton(ButtonId::Settings,false);
 Check(g_settings.trayButtons=="trash" && HasButton(ButtonId::Settings) && HasButton(ButtonId::Network),"removing works; fixed buttons cannot be removed");
 // cell planning: stacking pairs neighbours, odd one keeps a whole cell, widths from the catalogue
 std::vector<PlannedCell> cells;
 float end=PlanButtons({ButtonId::Desktop,ButtonId::Theme,ButtonId::Settings},40,1,true,100,&cells);
 Check(cells.size()==3 && cells[0].half==0 && cells[1].half==1 && cells[0].u0==cells[1].u0 && cells[2].half==-1,"stacked plan pairs the first two, the third stays whole");
 Check(std::fabs(cells[2].u1-cells[2].u0-32)<0.01f && std::fabs(end-cells[2].u1)<0.01f,"settings cell keeps its 32 px minimum and ends the group");
 PlanButtons({ButtonId::Desktop,ButtonId::Theme},40,1,false,0,&cells);
 Check(cells[0].half==-1 && cells[1].u0==cells[0].u1,"unstacked plan places buttons side by side");

 // recent files: newest first; unfinished downloads, hidden files and folders skipped
 std::wstring dir=AppDataDir()+L"\\recent-fixture";
 CreateDirectoryW(dir.c_str(),nullptr);
 WIN32_FIND_DATAW fd;HANDLE fh=FindFirstFileW((dir+L"\\*").c_str(),&fd);
 if(fh!=INVALID_HANDLE_VALUE){do{if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){std::wstring p=dir+L"\\"+fd.cFileName;SetFileAttributesW(p.c_str(),FILE_ATTRIBUTE_NORMAL);DeleteFileW(p.c_str());}}while(FindNextFileW(fh,&fd));FindClose(fh);}
 FILETIME now;GetSystemTimeAsFileTime(&now);
 auto make=[&](const wchar_t* name,int minutesAgo,DWORD attrs=0){
  std::wstring p=dir+L"\\"+name;HANDLE h=CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
  ULARGE_INTEGER t;t.LowPart=now.dwLowDateTime;t.HighPart=now.dwHighDateTime;t.QuadPart-=(ULONGLONG)minutesAgo*600000000ULL;
  FILETIME ft={t.LowPart,t.HighPart};SetFileTime(h,&ft,&ft,&ft);CloseHandle(h);
  if(attrs)SetFileAttributesW(p.c_str(),attrs);
 };
 make(L"old.txt",60*5);make(L"older.txt",60*24*3);make(L"new & shiny.zip",5);make(L"half.crdownload",1);make(L"part.part",1);make(L"temp.tmp",1);
 make(L"hidden.txt",0,FILE_ATTRIBUTE_HIDDEN);CreateDirectoryW((dir+L"\\folder").c_str(),nullptr);
 auto files=features::RecentFilesIn(dir,15);
 Check(files.size()==3,"recent files skip unfinished downloads, hidden files and folders");
 if(files.size()==3){
  Check(files[0].name==L"new & shiny.zip" && files[1].name==L"old.txt" && files[2].name==L"older.txt","recent files are newest first");
  Check(files[0].ago==L"5분 전" && files[1].ago==L"5시간 전" && files[2].ago==L"3일 전","recent files say how long ago in minutes, hours and days");
 }
 Check(features::RecentFilesIn(dir,1).size()==1,"recent files respect the maximum");
 Check(features::RecentFilesIn(dir+L"\\missing",15).empty(),"a missing folder gives an empty list");

 // trash drop: the dropped paths go to the Recycle Bin call, with undo (never a permanent delete)
 std::wstring a=dir+L"\\old.txt",b=dir+L"\\new & shiny.zip";
 size_t chars=a.size()+1+b.size()+1+1;
 HGLOBAL mem=GlobalAlloc(GHND,sizeof(DROPFILES)+chars*sizeof(wchar_t));
 auto* df=(DROPFILES*)GlobalLock(mem);df->pFiles=sizeof(DROPFILES);df->fWide=TRUE;
 wchar_t* list=(wchar_t*)((BYTE*)df+sizeof(DROPFILES));
 wcscpy_s(list,a.size()+1,a.c_str());wcscpy_s(list+a.size()+1,b.size()+1,b.c_str());
 GlobalUnlock(mem);
 Dock d(monitors[0].handle,9999,monitors[0].primary);
 lastDeleted.clear();
 bool ok=d.DropOnTrash((HDROP)mem);
 Check(ok && lastDeleted.size()==2 && lastDeleted[0]==a && lastDeleted[1]==b,"dropped files are passed to the Recycle Bin");
 Check(lastDeleteFunc==FO_DELETE && (lastDeleteFlags&FOF_ALLOWUNDO),"the trash drop recycles (undo allowed)");
 Check(GetFileAttributesW(a.c_str())!=INVALID_FILE_ATTRIBUTES,"the test never deletes for real");
 GlobalFree(mem);
 report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
}

int wmain(int argc,wchar_t** argv) {
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 winrt::init_apartment(winrt::apartment_type::single_threaded);
 if(!g_gfx.Init()){puts("GPU initialization failed");return 2;}
 g_sys.timeText=L"21:30";g_sys.timeShort=L"21:30";g_sys.dateText=L"2026-10-07";
 EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR h,HDC,LPRECT,LPARAM)->BOOL{
  MONITORINFO mi={sizeof(mi)};GetMonitorInfoW(h,&mi);
  monitors.push_back({h,mi.rcMonitor,(mi.dwFlags&MONITORINFOF_PRIMARY)!=0,true,(mi.dwFlags&MONITORINFOF_PRIMARY)?"actual-primary":"actual-secondary"});return TRUE;
 },0);
 const int actualCount=(int)monitors.size();
 if(argc>1 && wcscmp(argv[1],L"--preview-coupling")==0){
  report.open(AppDataDir()+L"\\preview-coupling.tsv");PreviewCouplingTests(actualCount);
  printf("RESULT preview_coupling_cases=%d checks=%d failures=%d; synthetic cursor, production Dock update and real DWM popup\n",cases,checks,failures);return failures?1:0;
 }
 if(argc>1 && wcscmp(argv[1],L"--trace")==0){TraceReproductions(actualCount);return 0;}
 if(argc>1 && wcscmp(argv[1],L"--render-only")==0){
  report.open(AppDataDir()+L"\\render-checks.tsv");
  RenderOptions(actualCount);
  printf("RENDER checks=%d failed=%d frames=%d; sample data only\n",checks,failures,actualCount*64);return failures?1:0;
 }
 report.open(AppDataDir()+L"\\matrix-results.tsv");
 UnitTests();
 monitors.push_back({(HMONITOR)(uintptr_t)0x1001,{-1920,0,0,1080},false,false,"simulated-left-negative"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1002,{0,-1440,2560,0},false,false,"simulated-above-negative"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1003,{3840,-200,5280,2360},false,false,"simulated-portrait"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1004,{0,0,1366,768},true,false,"simulated-small-laptop"});
 // common resolutions: the dock scales with the short side of the screen
 monitors.push_back({(HMONITOR)(uintptr_t)0x1005,{0,0,1280,720},true,false,"simulated-720p"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1006,{0,0,1920,1080},true,false,"simulated-1080p"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1007,{0,0,2560,1440},true,false,"simulated-1440p"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1008,{0,0,3840,2160},true,false,"simulated-4k"});
 monitors.push_back({(HMONITOR)(uintptr_t)0x1009,{0,0,3440,1440},true,false,"simulated-ultrawide"});
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(float size:{36.f,44.f,54.f})for(float mag:{1.f,1.7f,2.1f})for(int count:{12,40}) {
  ++cases;caseFailures=0;
  std::ostringstream label;label<<monitor.name<<" edge="<<edge<<" size="<<size<<" magnification="<<mag<<" apps="<<count;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;g_settings.iconSize=size;g_settings.magnification=mag;
  Apps(count);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);StateTests(d);SweepTests(d);
  if(size==44.f && mag==1.7f){SideExitTests(d);PinnedLayoutTests(d);SideExitShorterTests(monitor,cases);}
  if(monitor.actual && ((size==44 && mag==1.7f && count==12)||(size==54 && mag==2.1f && count==40)))ReachabilityTests(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
  if(cases%50==0){printf("Progress: %d cases, %d checks, %d failed checks\n",cases,checks,failures);fflush(stdout);}
 }
 // Focused setting combinations in addition to the unchanged baseline matrix:
 // small/default-density and large/crowded, all edges and monitor geometries.
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(int clock=0;clock<2;++clock)
 for(int spacing=0;spacing<3;++spacing)for(bool desktop:{false,true})for(bool crowded:{false,true}){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"options "<<monitor.name<<" edge="<<edge<<" clock="<<clock<<" spacing="<<spacing<<" desktop="<<desktop<<" crowded="<<crowded;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;g_settings.clockSize=clock;g_settings.iconSpacing=spacing;SetButton(ButtonId::Desktop,desktop);SetButton(ButtonId::Oled,((edge+clock+spacing)&1)!=0);SetButton(ButtonId::Theme,((edge+spacing)&1)==0);
  g_settings.iconSize=crowded?54.f:36.f;g_settings.magnification=crowded?2.1f:1.7f;
  Apps(crowded?40:12);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);OptionsTests(d);SweepTests(d);DragSpacingTests(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // Extremes: the largest icon size a settings file accepts, very many apps, every button on or
 // every optional button off. Fit, button pairing and click targets must still hold everywhere.
 struct Extreme{const char* name;float size,mag;int apps;bool buttons;int spacing,clock;};
 const Extreme extremes[]={{"max-icons-crowded",96,2.1f,60,true,2,1},{"ini-magnification-above-menu",96,2.5f,60,true,2,1},{"min-icons-crowded",32,1.7f,80,true,0,0},
  {"large-icons-few",54,2.1f,5,false,1,1},{"large-icons-all-buttons",54,2.1f,25,true,1,0}};
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(const auto& x:extremes){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"extreme "<<x.name<<" "<<monitor.name<<" edge="<<edge;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;g_settings.iconSize=x.size;g_settings.magnification=x.mag;
  g_settings.iconSpacing=x.spacing;g_settings.clockSize=x.clock;
  g_settings.magnification=std::min(g_settings.magnification,2.1f);   // what Settings::Load accepts
  g_settings.trayButtons=x.buttons?"desktop,theme,oled,awake,screenshot,quiet,trash,downloads,audio,sff":"";
  Apps(x.apps);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);
  // let the fit settle as it would after a few frames on screen
  for(int i=0;i<60;++i){d.ApplyFit(1.f/60.f);d.Layout(0);}
  Check(d.barU0>=-1 && d.barU1<=d.len+1,"extreme settings still fit the monitor edge");
  StateTests(d);OptionsTests(d);SweepTests(d);SideExitTests(d);
  if(monitor.actual)ReachabilityTests(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // Shape settings: glass thickness, distance from the edge, position along the edge.
 struct Shape{int thick,gap,offset;};
 const Shape shapes[]={{75,0,-100},{150,40,100},{120,20,-50},{100,6,0},{150,0,100},{75,40,-100}};
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(const auto& sh:shapes){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"shape thick="<<sh.thick<<" gap="<<sh.gap<<" offset="<<sh.offset<<" "<<monitor.name<<" edge="<<edge;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;g_settings.dockThickness=sh.thick;g_settings.edgeGap=sh.gap;g_settings.edgeOffset=sh.offset;
  SetButton(ButtonId::Oled,true);
  Apps(20);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);
  Check(std::fabs(d.margin-std::round(sh.gap*d.scale))<0.5f,"edge gap setting sets the glass distance from the edge");
  Check(d.margin+d.barThick<=d.T,"glass stays inside the dock window at any thickness and gap");
  Check(d.barU0>=-1 && d.barU1<=d.len+1,"shape settings keep the dock on the monitor");
  if(sh.offset==-100)Check(d.barU0<=8*d.scale+d.edgeReserve*0.5f+2,"offset -100 puts the dock against the start of the edge (leaving room to magnify)");
  if(sh.offset==100)Check(d.barU1>=d.len-8*d.scale-d.edgeReserve*0.5f-2,"offset 100 puts the dock against the end of the edge (leaving room to magnify)");
  if(sh.offset==0)Check(std::fabs((d.barU0+d.barU1)*0.5f-d.len*0.5f)<2,"offset 0 keeps the dock centred");
  // live change through Relayout: the window keeps its size, the layout follows the new values
  float before=d.barThick;int wx=d.winX,wy=d.winY,ww=d.winW,wh=d.winH;
  g_settings.dockThickness=sh.thick==150?75:150;g_settings.edgeGap=40-sh.gap;g_settings.edgeOffset=-sh.offset;
  Check(d.Relayout(),"thickness, gap and offset change without recreating the window");
  Check(d.winX==wx&&d.winY==wy&&d.winW==ww&&d.winH==wh,"relayout keeps the window rectangle");
  d.Layout(0);
  Check((g_settings.dockThickness>sh.thick)==(d.barThick>before),"thicker setting gives a thicker glass");
  Check(d.margin+d.barThick<=d.T,"glass stays inside the window after a live change");
  StateTests(d);OptionsTests(d);SweepTests(d);SideExitTests(d);
  if(monitor.actual)ReachabilityTests(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // Auto-hide off: shown without the pointer, put away only by a fullscreen app, and OLED mode
 // still hides it. (The fixture docks have no window, so no real app bar is registered.)
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(bool crowded:{false,true}){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"always-shown "<<monitor.name<<" edge="<<edge<<" crowded="<<crowded;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;g_settings.autoHide=false;
  if(crowded){g_settings.iconSize=54;g_settings.magnification=2.1f;}
  Apps(crowded?40:12);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);
  Advance(d,Outside(d.mon),1.0);
  Check(d.reveal.target==1 && d.reveal.x>0.95f,"auto-hide off: the dock is shown with the pointer elsewhere");
  Check(!IsRectEmpty(&d.curRegion),"auto-hide off: the shown dock takes input");
  fgEnabled=true;fgMonitor=monitor.handle;fgRect=d.mon;fgStyle=WS_POPUP;fgMaximized=false;
  Advance(d,Outside(d.mon),1.0);
  Check(d.reveal.target==0,"auto-hide off: a fullscreen app puts the dock away");
  fgEnabled=false;Advance(d,Outside(d.mon),1.0);
  Check(d.reveal.target==1,"auto-hide off: the dock returns when the fullscreen app is gone");
  // a browser maximized here, then F11 (mod.24), then Esc
  fgEnabled=true;fgMonitor=monitor.handle;fgMaximized=true;fgStyle=kChromeMaximized;fgRect=Grown(d.mon,8);fgClient=d.mon;
  Advance(d,Outside(d.mon),1.0);
  Check(d.reveal.target==1,"auto-hide off: a maximized browser leaves the dock up");
  fgStyle=kChromeF11;fgRect=d.mon;Advance(d,Outside(d.mon),1.0);
  Check(d.reveal.target==0 && d.FullscreenHere(),"auto-hide off: F11 in the maximized browser puts the dock away");
  fgStyle=kChromeMaximized;fgRect=Grown(d.mon,8);Advance(d,Outside(d.mon),1.0);
  Check(d.reveal.target==1 && !d.FullscreenHere(),"auto-hide off: leaving F11 brings the dock back");
  fgEnabled=false;fgMaximized=false;fgStyle=WS_POPUP;fgClient={};
  oledOn=true;Advance(d,Outside(d.mon),1.5);
  Check(d.reveal.target==0,"OLED mode hides the dock even with auto-hide off");
  oledOn=false;Advance(d,Outside(d.mon),1.0);
  Check(d.reveal.target==1,"leaving OLED mode shows the always-shown dock again");
  // every icon and button still works while it stays out
  for(size_t i=1;i<d.slots.size();i+=std::max<size_t>(1,d.slots.size()/6)){
   auto p=d.ScreenPointUV(d.slots[i].u,d.margin+d.barThick/2);Advance(d,p,0.4);
   float u,v;d.ScreenToUV(p,u,v);auto hit=d.HitTestUV(u,v);
   Check(hit.kind==Dock::HitSlot,"auto-hide off: icons stay reachable");
  }
  Advance(d,Outside(d.mon),1.0);
  Check(d.reveal.target==1,"auto-hide off: leaving the dock does not hide it");
  SideExitTests(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // Secondary monitors set to always show (the primary keeps auto-hiding): shown without the pointer,
 // no vsync-paced frames at rest, none while a fullscreen app runs on another monitor (and no screen
 // capture wanted then), and OLED mode on its monitor still hides it.
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"secondary always-shown "<<monitor.name<<" edge="<<edge;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;g_settings.secondaryAlwaysShown=true;
  Apps(12);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);
  Advance(d,Outside(d.mon),1.0);
  if(monitor.primary){
   Check(d.reveal.target==0,"secondary always shown: the primary dock keeps auto-hiding");
  }else{
   Check(d.reveal.target==1 && d.reveal.x>0.95f,"secondary always shown: shown with the pointer elsewhere");
   Check(!d.Busy(),"at rest it asks for no vsync-paced frames");
   auto p=d.ScreenPointUV(d.slots[1].u,d.margin+d.barThick/2);Advance(d,p,0.3);
   Check(d.Busy(),"the pointer on it brings vsync-paced frames back");
   Advance(d,Outside(d.mon),1.0);
   Check(!d.Busy(),"and it rests again when the pointer leaves");
   const MonitorCase* other=nullptr;
   for(const auto& m:monitors) if(m.handle!=monitor.handle){other=&m;break;}
   if(other){
    fgEnabled=true;fgMonitor=other->handle;fgRect=other->rect;fgStyle=WS_POPUP;fgMaximized=false;
    Advance(d,Outside(d.mon),1.0);
    Check(d.reveal.target==1,"a fullscreen app on another monitor leaves it shown");
    Check(!d.Busy() && simTime-d.lastCaptureWanted>0.9,"and pauses its glass: no frames, no capture wanted");
    fgEnabled=false;Advance(d,Outside(d.mon),0.2);
    Check(simTime-d.lastCaptureWanted<0.05,"the glass follows the screen again after the game");
    // a browser maximized on the other monitor goes F11 (mod.24): the same pause, and none while it is a window
    fgEnabled=true;fgMonitor=other->handle;fgMaximized=true;fgStyle=kChromeMaximized;fgRect=Grown(other->rect,8);fgClient=other->rect;
    Advance(d,Outside(d.mon),1.0);
    Check(simTime-d.lastCaptureWanted<0.05,"a maximized browser on another monitor does not pause the glass");
    fgStyle=kChromeF11;fgRect=other->rect;Advance(d,Outside(d.mon),1.0);
    Check(d.reveal.target==1 && !d.Busy() && simTime-d.lastCaptureWanted>0.9,"F11 in a maximized browser on another monitor pauses the glass");
    fgStyle=kChromeMaximized;fgRect=Grown(other->rect,8);Advance(d,Outside(d.mon),0.2);
    Check(simTime-d.lastCaptureWanted<0.05,"leaving F11 there: the glass follows the screen again");
    fgEnabled=false;fgMaximized=false;fgStyle=WS_POPUP;fgClient={};Advance(d,Outside(d.mon),0.2);
   }
   oledOn=true;Advance(d,Outside(d.mon),1.5);
   Check(d.reveal.target==0,"OLED mode on its monitor hides it");
   oledOn=false;Advance(d,Outside(d.mon),1.0);
   Check(d.reveal.target==1,"and it comes back");
  }
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // Maximized windows in front (mod.24): a browser's F11 over a maximized window, and the maximized windows that
 // are no fullscreen app, on every monitor and edge.
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"maximized "<<monitor.name<<" edge="<<edge;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;
  Apps(12);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);ZoomedTests(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // A window coming to the front over a dock that is up (mod.32), on every monitor and edge.
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"front "<<monitor.name<<" edge="<<edge;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;
  Apps(12);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);FrontTests(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // Tray icons (Settings::trayIcons): between the chevron and network/volume, two to a cell when the
 // bar is thick enough, each drawn cell its own click target, nothing overlapping.
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(int count:{1,2,5}){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"tray icons count="<<count<<" "<<monitor.name<<" edge="<<edge;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;
  Apps(12);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);
  d.notifyCells.clear();
  for(int i=0;i<count;++i){Dock::NotifyCell c;c.key=1000+i;c.tip=L"tip";d.notifyCells.push_back(c);}
  d.HotkeyReveal(simTime);Advance(d,Outside(d.mon),1.0);   // shown and laid out
  Check(d.notifyCells.size()==(size_t)count,"injected tray icons stay (no tray host runs in the matrix)");
  float trayEnd=d.cells[Dock::HitTray].u1,netStart=std::min(d.Cell_(ButtonId::Network).u0,d.Cell_(ButtonId::Volume).u0);
  std::vector<D2D1_RECT_F> seen;
  for(int i=0;i<count;++i){
   const auto& c=d.notifyCells[i];
   Check(c.u0>=trayEnd-0.01f && c.u1<=netStart+0.01f,"tray icons sit between the chevron and network/volume");
   Check(d.stackButtons ? ((count%2==1 && i==count-1) ? c.half<0 : c.half==i%2) : c.half<0,"two to a cell when stacked, an odd last one alone");
   auto r=d.NotifyRect(i);
   Check(std::min(r.right-r.left,r.bottom-r.top)>=22*d.scale-1,"tray icons keep a comfortable click size");
   for(auto& o:seen){
    float ix=std::min(r.right,o.right)-std::max(r.left,o.left),iy=std::min(r.bottom,o.bottom)-std::max(r.top,o.top);
    Check(ix<=0.5f||iy<=0.5f,"tray icons never overlap");
   }
   seen.push_back(r);
   POINT local={(LONG)((r.left+r.right)/2),(LONG)((r.top+r.bottom)/2)};
   float u,v;d.ClientToUV(local.x,local.y,u,v);
   auto hit=d.HitTestUV(u,v);
   Check(hit.kind==Dock::HitNotify && hit.notify==i,"each tray icon is its own click target");
   Check(PtInRect(&d.curRegion,local)!=0,"tray icons are in the active input region");
  }
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // The battery cell (laptops): between the sensor cell and the small buttons, its own click target,
 // its icon larger than the small buttons'; none on a desktop.
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(bool crowded:{false,true}){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"battery crowded="<<crowded<<" "<<monitor.name<<" edge="<<edge;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;
  if(crowded){g_settings.iconSize=54;g_settings.magnification=2.1f;}
  battery::State st;st.present=true;st.percent=crowded?15:64;st.charging=st.plugged=!crowded;
  g_sys.SetBattery(st);
  Apps(crowded?40:8);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);
  for(int i=0;i<60;++i){d.ApplyFit(1.f/60.f);d.Layout(0);}
  const auto metrics=d.MeasureMetrics(d.fit);
  const auto& cell=d.cells[Dock::HitBattery];
  Check(cell.u1-cell.u0>=BatteryIconWidth(metrics.icon,d.scale)*0.99f || IsVertical(g_settings.edge),"battery cell is as wide as its icon");
  Check(cell.u0>=d.cells[Dock::HitSensor].u1-0.01f,"battery cell follows the sensor cell");
  Check(BatteryIconWidth(metrics.icon,d.scale)>=1.5f*17*d.scale-0.5f,"battery icon is half again as wide as the small buttons' glyphs");
  Check(d.barU0>=-1 && d.barU1<=d.len+1,"the battery cell keeps the dock on the monitor");
  ResetHidden(d);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),0.8);AssertHitGeometry(d);
  auto p=d.ScreenPointUV((cell.u0+cell.u1)/2,d.margin+d.barThick/2);Advance(d,p,1.0);
  Check(d.hoverKind==Dock::HitBattery && d.reveal.target==1,"the battery cell is hovered and keeps the dock out");
  Check(d.panelAlpha.target==0,"battery hover does not open the sensor panel");
  g_sys.SetBattery({});Advance(d,p,0.5);
  Check(d.cells[Dock::HitBattery].u1<=d.cells[Dock::HitBattery].u0+0.01f,"no battery: the cell takes no space");
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // Quiet button and the reveal shortcut, on every monitor and edge.
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"features "<<monitor.name<<" edge="<<edge;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;
  Apps(10);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);
  if(monitor.primary){
   auto& s=d.slots[3];
   features::SetQuiet(1e300);
   g_apps.attentionEvents.push_back({s.app->id});Advance(d,Outside(d.mon),0.1);g_apps.attentionEvents.clear();
   Check(s.bounceCount==0 && !s.bounceLoop,"quiet: a notification does not bounce the icon");
   features::SetQuiet(0);
   g_apps.attentionEvents.push_back({s.app->id});Advance(d,Outside(d.mon),0.1);g_apps.attentionEvents.clear();
   Check(s.bounceCount>0,"quiet off: notifications bounce again");
   ResetHidden(d);
  }
  d.HotkeyReveal(simTime);Advance(d,Outside(d.mon),1.0);
  Check(d.reveal.target==1 && d.reveal.x>0.9f,"shortcut: the dock comes out without the pointer at the edge");
  Advance(d,Outside(d.mon),3.0);
  Check(d.reveal.target==0,"shortcut: it goes away again when the pointer never came");
  d.HotkeyReveal(simTime);Advance(d,Outside(d.mon),0.5);d.HotkeyReveal(simTime);Advance(d,Outside(d.mon),0.5);
  Check(d.reveal.target==0,"shortcut pressed again puts the dock away");
  d.HotkeyReveal(simTime);
  auto p=d.ScreenPointUV(d.slots[1].u,d.margin+d.barThick/2);Advance(d,p,4.0);
  Check(d.reveal.target==1,"shortcut: the dock stays while the pointer is on it");
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 // Button lists: none, one, odd, every button, reversed, mixed; with crowded and sparse docks.
 const char* lists[]={"","trash","awake,quiet,screenshot","desktop,theme,oled,awake,screenshot,quiet,trash,downloads,audio,sff",
  "sff,audio,downloads,trash,quiet,screenshot,awake,oled,theme,desktop","downloads,desktop"};
 for(const auto& monitor:monitors)for(int edge=0;edge<4;++edge)for(int li=0;li<6;++li)for(bool crowded:{false,true}){
  ++cases;caseFailures=0;
  std::ostringstream label;label<<"buttons list="<<li<<" crowded="<<crowded<<" "<<monitor.name<<" edge="<<edge;caseLabel=label.str();
  g_settings=Settings{};g_settings.edge=(Edge)edge;g_settings.trayButtons=lists[li];
  if(crowded){g_settings.iconSize=54;g_settings.magnification=2.1f;}
  Apps(crowded?40:8);Dock d(monitor.handle,cases,monitor.primary);InitializeLayout(d);
  for(int i=0;i<60;++i){d.ApplyFit(1.f/60.f);d.Layout(0);}
  Check(d.barU0>=-1 && d.barU1<=d.len+1,"any button list keeps the dock on the monitor");
  OptionsTests(d);SweepTests(d);
  // a list change applies in place
  g_settings.trayButtons=lists[(li+1)%6];
  Check(d.Relayout(),"changing the button list relays out without recreating the window");
  ResetHidden(d);Advance(d,EdgePoint(d.mon,g_settings.edge,0.5f,0),0.8);AssertHitGeometry(d);
  report<<(caseFailures?"CASE_FAIL":"CASE_PASS")<<"\t"<<caseLabel<<"\t"<<caseFailures<<"\n";
 }
 RenderOptions(actualCount);
 report<<"TOTAL\t"<<cases<<"\t"<<checks<<"\t"<<failures<<"\n";report.close();
 printf("RESULT cases=%d checks=%d failed_checks=%d; synthetic input tests; render fixtures=%d\n",cases,checks,failures,actualCount*64);
 return failures?1:0;
}
