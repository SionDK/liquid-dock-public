// Real DWM/Win32 integration, restricted to disposable windows in this process.
// No synthetic keyboard/mouse input, no Explorer or installed settings changes.
// Self-contained: the preview's own code (window_preview.cpp) with stand-ins for the dock's
// AppModel (apps*.cpp, not part of this test): activation records which window a click chose;
// closing asks the window as its own close button does (SC_CLOSE), which closes these test windows.
// tests\run_preview_live.bat builds and runs it.
// Arguments: "front" (whatever is in front now: the console it was started from) or the
// process id of the window the caller saw in front; then the folder for the popup's pictures.
#include "../src/common.h"
#include "../src/apps.h"
#include <dwmapi.h>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <set>
#define private public
#include "../src/window_preview.h"
#undef private
#include "../src/window_preview.cpp"
Settings g_settings;
AppModel g_apps;
static HWND activated=nullptr;
void AppModel::ActivateWindow(HWND h){activated=h;}
static std::vector<HWND> askedToClose;
void AppModel::AskToClose(HWND h,const std::wstring&){askedToClose.push_back(h);PostMessageW(h,WM_SYSCOMMAND,SC_CLOSE,0);}
// The preview's dock.log lines go to this test's output.
void Log(const wchar_t* fmt,...){
 wchar_t w[1024];va_list ap;va_start(ap,fmt);vswprintf(w,1024,fmt,ap);va_end(ap);
 char u[3072];if(WideCharToMultiByte(CP_UTF8,0,w,-1,u,sizeof(u),nullptr,nullptr))printf("LOG %s\n",u);fflush(stdout);
}
static int checks=0, failures=0;
static COLORREF liveColor=RGB(32,164,215);
static void Check(bool ok,const char* msg){++checks;if(!ok){++failures;printf("FAIL %s\n",msg);fflush(stdout);}}
static void Pump(int ms){auto end=GetTickCount64()+ms;do{MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);}while(GetTickCount64()<end);}
static LRESULT CALLBACK SourceProc(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);HBRUSH b=CreateSolidBrush(liveColor);FillRect(dc,&r,b);DeleteObject(b);EndPaint(h,&ps);return 0;}return DefWindowProcW(h,m,w,l);
}
static bool Sample(WindowPreview& p,int index,COLORREF expected){
 RECT r=p.entries[index].image;HDC dc=GetDC(nullptr);COLORREF got=GetPixel(dc,p.bounds.left+(r.left+r.right)/2,p.bounds.top+(r.top+r.bottom)/2);ReleaseDC(nullptr,dc);
 bool ok=got!=CLR_INVALID&&abs((int)GetRValue(got)-GetRValue(expected))<=8&&abs((int)GetGValue(got)-GetGValue(expected))<=8&&abs((int)GetBValue(got)-GetBValue(expected))<=8;
 printf("PIXEL expected=%06lx actual=%06lx match=%d\n",expected,got,ok);return ok;
}
static void SaveBmp(RECT r,const std::wstring& path){
 int w=r.right-r.left,h=r.bottom-r.top;HDC screen=GetDC(nullptr),mem=CreateCompatibleDC(screen);
 BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
 void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(mem,bitmap);
 BitBlt(mem,0,0,w,h,screen,r.left,r.top,SRCCOPY|CAPTUREBLT);GdiFlush();
 BITMAPFILEHEADER bf{};bf.bfType=0x4d42;bf.bfOffBits=sizeof(bf)+sizeof(BITMAPINFOHEADER);bf.bfSize=bf.bfOffBits+w*h*4;
 std::ofstream f(path,std::ios::binary);f.write((char*)&bf,sizeof(bf));f.write((char*)&bi.bmiHeader,sizeof(bi.bmiHeader));f.write((char*)pixels,w*h*4);
 SelectObject(mem,old);DeleteObject(bitmap);DeleteDC(mem);ReleaseDC(nullptr,screen);
}
// Pages until card `index` is on the popup and DWM has drawn the page; false if it cannot get there.
static bool Reach(WindowPreview& p,int index){
 while(index<p.first){int f=p.first;p.ChangePage(-1);if(p.first==f)return false;}
 while(index>=p.first+p.capacity){int f=p.first;p.ChangePage(1);if(p.first==f)return false;}
 Pump(150);DwmFlush();return true;
}
// What DWM reports of a window's thumbnail, through a relationship of another of this test's windows.
static HRESULT DwmSize(HWND destination,HWND source,SIZE* size){
 HTHUMBNAIL t=nullptr;HRESULT hr=DwmRegisterThumbnail(destination,source,&t);
 if(SUCCEEDED(hr))hr=DwmQueryThumbnailSourceSize(t,size);
 if(t)DwmUnregisterThumbnail(t);return hr;
}
// For the record, apart from the dock's code: what a relationship of a minimized window draws when it is simply
// shown at a fixed size in a probe window of this test's own, whatever size DWM reports for it - the image DWM
// kept, black, or nothing (the probe's own color shows through).
static const COLORREF kProbeColor=RGB(200,40,160);
static LRESULT CALLBACK ProbeProc(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);HBRUSH b=CreateSolidBrush(kProbeColor);FillRect(dc,&r,b);DeleteObject(b);EndPaint(h,&ps);return 0;}
 return DefWindowProcW(h,m,w,l);
}
static bool Near(COLORREF a,COLORREF b){return a!=CLR_INVALID&&abs((int)GetRValue(a)-GetRValue(b))<=8&&abs((int)GetGValue(a)-GetGValue(b))<=8&&abs((int)GetBValue(a)-GetBValue(b))<=8;}
static COLORREF DrawnBy(HWND probe,HWND source){
 HTHUMBNAIL t=nullptr;if(FAILED(DwmRegisterThumbnail(probe,source,&t)))return CLR_INVALID;
 DWM_THUMBNAIL_PROPERTIES props{};props.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_VISIBLE|DWM_TNP_OPACITY|DWM_TNP_SOURCECLIENTAREAONLY;
 props.rcDestination={20,20,280,190};props.fVisible=TRUE;props.opacity=255;props.fSourceClientAreaOnly=FALSE;
 COLORREF got=CLR_INVALID;
 if(SUCCEEDED(DwmUpdateThumbnailProperties(t,&props))){Pump(120);DwmFlush();POINT c{150,105};ClientToScreen(probe,&c);HDC dc=GetDC(nullptr);got=GetPixel(dc,c.x,c.y);ReleaseDC(nullptr,dc);}
 DwmUnregisterThumbnail(t);return got;
}
static std::string Drawn(COLORREF got,COLORREF kept){
 char text[48];
 if(got==CLR_INVALID)return "no relationship";
 if(Near(got,kept))return "its kept image";
 if(Near(got,kProbeColor))return "nothing";
 if(GetRValue(got)<=16&&GetGValue(got)<=16&&GetBValue(got)<=16)return "black";
 snprintf(text,sizeof(text),"other %06lx",got);return text;
}
// windows[5] and [6] are ordinary windows (a taskbar button, as an app's), the others tool windows;
// [1] and [6] start minimized (never on screen), [4] and [5] are minimized after they were.
static bool Ordinary(int i){return i==5||i==6;}
static bool StartsMinimized(int i){return i==1||i==6;}
int wmain(int argc,wchar_t** argv){
 if(argc!=3)return 2;
 HWND original=GetForegroundWindow();DWORD pid=0;GetWindowThreadProcessId(original,&pid);
 bool front=wcscmp(argv[1],L"front")==0;
 if(!original||(!front&&pid!=wcstoul(argv[1],nullptr,10))){puts("Foreground changed; no test windows created.");return 2;}
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 ANIMATIONINFO animation{sizeof(animation)};SystemParametersInfoW(SPI_GETANIMATION,sizeof(animation),&animation,0);
 printf("SYSTEM minimize animation=%d\n",animation.iMinAnimate);
 std::vector<RECT> monitors;EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT r,LPARAM p)->BOOL{((std::vector<RECT>*)p)->push_back(*r);return TRUE;},(LPARAM)&monitors);
 WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=SourceProc;wc.lpszClassName=L"LiquidDockPreviewFixture";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
 WNDCLASSW pc=wc;pc.lpfnWndProc=ProbeProc;pc.lpszClassName=L"LiquidDockPreviewProbe";RegisterClassW(&pc);
 int cases=0;
 for(size_t mi=0;mi<monitors.size();++mi)for(Edge edge:{Edge::Bottom,Edge::Left,Edge::Right,Edge::Top}){
  liveColor=RGB(32,164,215);
  RECT m=monitors[mi];int cx=(m.left+m.right)/2,cy=(m.top+m.bottom)/2;int before=failures;
  std::vector<HWND> windows;
  for(int i=0;i<12;++i){std::wstring title=L"Preview fixture "+std::to_wstring(i+1)+L" — 창 선택 확인";
   HWND h=CreateWindowExW(Ordinary(i)?0:WS_EX_TOOLWINDOW,wc.lpszClassName,title.c_str(),WS_OVERLAPPEDWINDOW,m.left+250+i*8,m.top+180+i*8,520,340,nullptr,nullptr,wc.hInstance,nullptr);
   if(!h){puts("Window creation failed");return 2;}windows.push_back(h);ShowWindow(h,StartsMinimized(i)?SW_SHOWMINNOACTIVE:SW_SHOWNOACTIVATE);
  }
  // Known protected own-process source: do not try to recover its pixels.
  SetWindowDisplayAffinity(windows[2],WDA_EXCLUDEFROMCAPTURE);
  Pump(200);DwmFlush();
  // On screen in liveColor until now: minimized, DWM keeps that image of them (the one the taskbar shows).
  COLORREF keptColor=liveColor;
  ShowWindow(windows[4],SW_SHOWMINNOACTIVE);ShowWindow(windows[5],SW_SHOWMINNOACTIVE);Pump(500);
  if(edge==Edge::Bottom){   // the probe, once per monitor, before the preview opens
   HWND probe=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,L"LiquidDockPreviewProbe",L"",WS_POPUP,cx-150,cy-105,300,210,nullptr,nullptr,wc.hInstance,nullptr);
   if(probe){
    ShowWindow(probe,SW_SHOWNOACTIVATE);Pump(100);DwmFlush();
    for(int i:{1,4,5,6})printf("PROBE monitor=%zu window=%d (%s, %s): a relationship shown at 260x170 draws %s\n",mi,i,Ordinary(i)?"ordinary":"tool window",
     StartsMinimized(i)?"started minimized":"minimized after it was on screen",Drawn(DrawnBy(probe,windows[i]),keptColor).c_str());
    fflush(stdout);DestroyWindow(probe);Pump(50);
   }
  }
  auto app=std::make_shared<AppItem>();app->name=L"LiquidDock 미리보기 검증";app->windows=windows;
  RECT anchor{};switch(edge){case Edge::Left:anchor={m.left,cy-35,m.left+170,cy+35};break;case Edge::Right:anchor={m.right-170,cy-35,m.right,cy+35};break;case Edge::Top:anchor={cx-35,m.top,cx+35,m.top+170};break;case Edge::Bottom:anchor={cx-35,m.bottom-170,cx+35,m.bottom};break;}
  POINT cursor{(anchor.left+anchor.right)/2,(anchor.top+anchor.bottom)/2};float scale=std::max(.72f,std::min(m.right-m.left,m.bottom-m.top)/1440.f);
  {
   WindowPreview p;
   p.Update(windows[0],app,anchor,m,edge,scale,cursor,0,true);
   Check(!p.Open(),"dwell does not open immediately");
   p.Update(windows[0],app,anchor,m,edge,scale,cursor,.5,true);Pump(150);
   Check(p.Open(),"real preview opens");
   Check(p.bounds.left>=m.left&&p.bounds.right<=m.right&&p.bounds.top>=m.top&&p.bounds.bottom<=m.bottom,"popup stays within monitor");
   Check(edge==Edge::Left?p.bounds.left>anchor.right:edge==Edge::Right?p.bounds.right<anchor.left:edge==Edge::Top?p.bounds.top>anchor.bottom:p.bounds.bottom<anchor.top,"popup clears dock edge");
   DWORD affinity=0;Check(GetWindowDisplayAffinity(p.hwnd,&affinity)&&affinity==WDA_EXCLUDEFROMCAPTURE,"production popup requests capture exclusion");
   // Only the test popup becomes capturable so its actual DWM output can be measured.
   SetWindowDisplayAffinity(p.hwnd,WDA_NONE);Pump(100);DwmFlush();
   Check(p.entries.size()==12,"all real windows present");
   Check(p.entries[0].thumbnail!=nullptr,"real DWM relationship created");
   Check(Sample(p,0,liveColor),"DWM actually renders source pixels");
   liveColor=RGB(225,127,40);InvalidateRect(windows[0],nullptr,FALSE);Pump(150);DwmFlush();
   Check(Sample(p,0,liveColor),"DWM updates live source contents");
   if(p.capacity>2)Check(!p.entries[2].thumbnail&&!p.entries[2].fallback.empty(),"protected source has title fallback");
   // Minimized windows, as on the taskbar: the image DWM kept of one that was on screen (it stays as it
   // was: liveColor has changed since); for one that never was, the card DWM makes instead (its icon on a
   // grey ground, 300x188 at 125 % on 25H2 - what the taskbar shows too, seen 2026-10-27). Checked on the
   // ordinary windows, the kind the dock lists (apps.cpp leaves tool windows out); what DWM reported for
   // each, tool windows too, is printed for the record.
   for(int i:{1,4,5,6}){
    SIZE s{};HRESULT hr=DwmSize(windows[11],windows[i],&s);
    RECT own{};GetWindowRect(windows[i],&own);WINDOWPLACEMENT wp{sizeof(wp)};GetWindowPlacement(windows[i],&wp);RECT n=wp.rcNormalPosition;
    bool reached=Reach(p,i);auto& e=p.entries[i];bool image=reached&&e.thumbnail&&e.fallback.empty();
    printf("MINIMIZED window=%d %s, %s: iconic=%d dwm=%ldx%ld hr=0x%08lx own=%ldx%ld normal=%ldx%ld preview=%s\n",i,Ordinary(i)?"ordinary":"tool window",
     StartsMinimized(i)?"started minimized":"minimized after it was on screen",IsIconic(windows[i])?1:0,s.cx,s.cy,(unsigned long)hr,
     own.right-own.left,own.bottom-own.top,n.right-n.left,n.bottom-n.top,!reached?"unreachable":image?"image":"placeholder");
    fflush(stdout);
    Check(IsIconic(windows[i]),"a minimized window stays minimized with its preview open");
    if(!Ordinary(i)){if(image)Sample(p,i,keptColor);continue;}   // for the record only
    if(StartsMinimized(i)){
     Check(image,"a window started minimized shows the card DWM makes for it (its icon), as the taskbar does");
     if(image)Sample(p,i,keptColor);   // for the record: the card's middle, its icon
    }else{
     Check(image,"a window minimized after it was on screen shows the image DWM kept");
     Check(image&&Sample(p,i,keptColor),"...in the color it had when it was minimized");
    }
   }
   p.ChangePage(-100);Pump(150);DwmFlush();
   SaveBmp(p.bounds,std::wstring(argv[2])+L"\\preview-m"+std::to_wstring(mi)+L"-e"+std::to_wstring((int)edge)+L".bmp");
   POINT bridge{(p.bridge.left+p.bridge.right)/2,(p.bridge.top+p.bridge.bottom)/2};
   p.Update(windows[0],nullptr,{},m,edge,scale,bridge,.7,true);Check(p.Open()&&p.Contains(bridge),"gap crossing keeps real popup open");
   std::set<HWND> reached;for(int page=0;page<12;++page){for(int j=p.first;j<std::min(p.first+p.capacity,12);++j)reached.insert(p.entries[j].source);int old=p.first;p.ChangePage(1);if(p.first==old)break;}
   Check(reached.size()==12,"paging reaches all 12 windows");p.ChangePage(-100);
   // Closing from the preview, as on the taskbar: the close button of the card under the pointer, then a
   // middle click on another card. The windows go, their cards with them; the popup stays with the rest.
   {
    askedToClose.clear();double t=1;
    auto hoverOn=[&](RECT r){POINT c{p.bounds.left+(r.left+r.right)/2,p.bounds.top+(r.top+r.bottom)/2};p.Update(windows[0],app,anchor,m,edge,scale,c,t+=.25,true);};
    auto at=[](RECT r){return MAKELPARAM((r.left+r.right)/2,(r.top+r.bottom)/2);};
    bool reached7=Reach(p,7);
    if(reached7){
     hoverOn(p.CloseRect(p.entries[7]));Pump(80);
     Check(p.hot==7&&p.hotClose==7,"the pointer on a card's close button: that card and its button are under it");
     SaveBmp(p.bounds,std::wstring(argv[2])+L"\\preview-m"+std::to_wstring(mi)+L"-e"+std::to_wstring((int)edge)+L"-close.bmp");
     SendMessageW(p.hwnd,WM_LBUTTONDOWN,0,at(p.CloseRect(p.entries[7])));SendMessageW(p.hwnd,WM_LBUTTONUP,0,at(p.CloseRect(p.entries[7])));
     Pump(150);
     Check(askedToClose.size()==1&&askedToClose[0]==windows[7]&&!IsWindow(windows[7]),"the close button closes its window");
     hoverOn(p.entries[0].card);Pump(50);
     Check(p.Open()&&p.entries.size()==11&&activated==nullptr,"its card goes, the popup stays with the rest, nothing activated");
    } else Check(false,"fixture: card 7 reachable");
    p.ChangePage(-100);bool reached8=Reach(p,7);   // windows[8]'s card is now the 8th
    if(reached8&&p.entries[7].source==windows[8]){
     LPARAM card8=at(p.entries[7].image);
     SendMessageW(p.hwnd,WM_MBUTTONDOWN,0,card8);SendMessageW(p.hwnd,WM_MBUTTONUP,0,card8);Pump(150);
     Check(askedToClose.size()==2&&askedToClose[1]==windows[8]&&!IsWindow(windows[8]),"a middle click on a card closes its window");
     hoverOn(p.entries[0].card);Pump(50);
     Check(p.Open()&&p.entries.size()==10&&activated==nullptr,"...and the popup stays with the rest");
    } else Check(false,"fixture: windows[8]'s card reachable");
    p.ChangePage(-100);Pump(50);
   }
   // A source disappearing between press/release must never activate its replacement.
   RECT card=p.entries[0].card;LPARAM point=MAKELPARAM((card.left+card.right)/2,(card.top+card.bottom)/2);
   SendMessageW(p.hwnd,WM_LBUTTONDOWN,0,point);DestroyWindow(windows[0]);app->windows.erase(app->windows.begin());
   // Owner destruction also destroys the popup; this is an ownership-lifetime test.
   Check(!p.hwnd&&!p.Open(),"owner destruction destroys preview without stale HWND");
   p.Dismiss();
   // Reopen using another surviving owner, then choose the minimized source via the actual input handler.
   p.candidate.reset();p.Update(windows[3],app,anchor,m,edge,scale,cursor,1,true);p.Update(windows[3],app,anchor,m,edge,scale,cursor,1.5,true);Pump(100);
   Check(p.Open(),"reopen after owner destruction");
   if(p.Open()){
    card=p.entries[0].card;point=MAKELPARAM((card.left+card.right)/2,(card.top+card.bottom)/2);
    SendMessageW(p.hwnd,WM_LBUTTONDOWN,0,point);SendMessageW(p.hwnd,WM_LBUTTONUP,0,point);Pump(250);
    Check(activated==windows[1],"a click on a minimized window's card chooses that window");Check(!p.Open(),"selection dismisses popup");
   }
   activated=nullptr;
  }
  for(HWND h:windows)if(IsWindow(h))DestroyWindow(h);
  if(IsWindow(original))SetForegroundWindow(original);Pump(80);
  printf("LIVE_PREVIEW monitor=%zu edge=%d checks=%d failed=%d\n",mi,(int)edge,checks,failures-before);fflush(stdout);++cases;
 }
 printf("RESULT live_preview_cases=%d checks=%d failures=%d\n",cases,checks,failures);
 CoUninitialize();return failures?1:0;
}
