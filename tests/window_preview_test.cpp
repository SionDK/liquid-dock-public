// Production preview logic with an in-memory window/DWM model. No real windows,
// input, taskbar settings, source capture, or user app activation are involved.
#include "../src/common.h"
#include "../src/apps.h"
#include <dwmapi.h>
#include <map>
#include <set>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#define private public
#include "../src/window_preview.h"
#undef private

struct Source {
    DWORD pid = 3; bool alive = true, minimized = false, protectedContent = false;
    SIZE dwm{1920, 1080};       // what DWM reports for its thumbnail; while minimized, the image it kept
    bool dwmFails = false;      // DwmQueryThumbnailSourceSize fails
    bool updateFails = false;   // DwmUpdateThumbnailProperties fails
    bool rectFails = false;     // GetWindowRect fails (the window is going away)
    RECT normalRect{100, 100, 1300, 800};
    RECT minimizedRect{-32000, -32000, -31840, -31972};   // a minimized window itself: its caption, off screen (100 %)
};
static std::map<HWND, Source> sources;
static std::set<HTHUMBNAIL> thumbnails;
static std::map<HTHUMBNAIL, DWM_THUMBNAIL_PROPERTIES> lastProps;
static std::map<HWND, int> registeredFor;
static std::vector<std::wstring> logs;
static bool shown = false, failDwm = false;
static int updates = 0, checks = 0, activations = 0, registrations = 0, sourceTouched = 0;
static HWND foreground = (HWND)0x9090;
static SHORT escapeState = 0;
static WindowPreview* current = nullptr;
static HWND captured = nullptr;
static LONG_PTR WINAPI FakeUserData(HWND, int) { return reinterpret_cast<LONG_PTR>(current); }
static HWND WINAPI FakeCapture(HWND h) { HWND old = captured; captured = h; return old; }
static HWND WINAPI FakeCaptured() { return captured; }
static BOOL WINAPI FakeRelease() { captured = nullptr; return TRUE; }
static BOOL WINAPI FakeIsWindow(HWND h) { return sources.count(h) && sources[h].alive; }
static BOOL WINAPI FakeVisible(HWND h) { return h == (HWND)0x8080 ? shown : FakeIsWindow(h); }
// Showing, restoring or moving a source window would be the preview touching it.
static BOOL WINAPI FakeShow(HWND h, int cmd) { if (sources.count(h)) { ++sourceTouched; return TRUE; } shown = cmd != SW_HIDE; return TRUE; }
static DWORD WINAPI FakePid(HWND h, LPDWORD pid) { if (pid) *pid = FakeIsWindow(h) ? sources[h].pid : 0; return 1; }
static BOOL WINAPI FakeIconic(HWND h) { return sources[h].minimized; }
static BOOL WINAPI FakeAffinity(HWND h, DWORD* value) { *value = sources[h].protectedContent ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE; return TRUE; }
static HRESULT WINAPI FakeAttribute(HWND, DWORD, PVOID value, DWORD) { *static_cast<DWORD*>(value) = 0; return S_OK; }
static HRESULT WINAPI FakeRegister(HWND, HWND h, HTHUMBNAIL* out) {
    if (failDwm) return E_FAIL;
    ++registrations; ++registeredFor[h]; *out = (HTHUMBNAIL)h; thumbnails.insert(*out); return S_OK;
}
static HRESULT WINAPI FakeUnregister(HTHUMBNAIL h) { thumbnails.erase(h); lastProps.erase(h); return S_OK; }
static HRESULT WINAPI FakeSize(HTHUMBNAIL h, SIZE* size) {
    auto it = sources.find((HWND)h);
    if (it == sources.end() || it->second.dwmFails) return E_FAIL;
    *size = it->second.dwm; return S_OK;
}
static HRESULT WINAPI FakeUpdate(HTHUMBNAIL h, const DWM_THUMBNAIL_PROPERTIES* props) {
    ++updates;
    if (props->rcDestination.right <= props->rcDestination.left || props->rcDestination.bottom <= props->rcDestination.top) std::abort();
    auto it = sources.find((HWND)h);
    if (it != sources.end() && it->second.updateFails) return E_FAIL;
    lastProps[h] = *props;
    return S_OK;
}
// Any other way of restoring, showing, moving or activating a source window, or of sending it a command
// (the preview uses none of them today; these catch one that comes).
static BOOL WINAPI FakeTouch(HWND h) { if (sources.count(h)) ++sourceTouched; return TRUE; }
[[maybe_unused]] static BOOL WINAPI FakePlacement(HWND h, const WINDOWPLACEMENT*) { return FakeTouch(h); }
[[maybe_unused]] static BOOL WINAPI FakeShowAsync(HWND h, int) { return FakeTouch(h); }
[[maybe_unused]] static BOOL WINAPI FakeMove(HWND h, int, int, int, int, BOOL) { return FakeTouch(h); }
[[maybe_unused]] static BOOL WINAPI FakePost(HWND h, UINT, WPARAM, LPARAM) { return FakeTouch(h); }
[[maybe_unused]] static LRESULT WINAPI FakeSend(HWND h, UINT, WPARAM, LPARAM) { FakeTouch(h); return 0; }
static BOOL WINAPI FakeWindowRect(HWND h, LPRECT r) {
    auto it = sources.find(h);
    if (it == sources.end() || it->second.rectFails) return FALSE;
    *r = it->second.minimized ? it->second.minimizedRect : it->second.normalRect; return TRUE;
}
static int WINAPI FakeClassName(HWND, LPWSTR name, int count) { wcscpy_s(name, count, L"FixtureClass"); return 12; }
static BOOL WINAPI FakePos(HWND h, HWND, int, int, int, int, UINT) { if (sources.count(h)) ++sourceTouched; return TRUE; }
static int WINAPI FakeRegion(HWND, HRGN r, BOOL) { DeleteObject(r); return 1; }
static BOOL WINAPI FakeInvalidate(HWND, const RECT*, BOOL) { return TRUE; }
static int WINAPI FakeText(HWND, LPWSTR text, int count) { wcscpy_s(text, count, L"Fixture window"); return 14; }
static HWND WINAPI FakeForeground() { return foreground; }
static SHORT WINAPI FakeKey(int key) { return key == VK_ESCAPE ? escapeState : 0; }
static BOOL WINAPI FakeDestroy(HWND) { return TRUE; }
static void FakeLog(const wchar_t* fmt, ...) {
    wchar_t line[1024]; va_list ap; va_start(ap, fmt); vswprintf(line, 1024, fmt, ap); va_end(ap);
    logs.push_back(line);
}

#define IsWindow FakeIsWindow
#define IsWindowVisible FakeVisible
#define ShowWindow FakeShow
#define GetWindowThreadProcessId FakePid
#define IsIconic FakeIconic
#define GetWindowDisplayAffinity FakeAffinity
#define DwmGetWindowAttribute FakeAttribute
#define DwmRegisterThumbnail FakeRegister
#define DwmUnregisterThumbnail FakeUnregister
#define DwmQueryThumbnailSourceSize FakeSize
#define DwmUpdateThumbnailProperties FakeUpdate
#define GetWindowRect FakeWindowRect
#define GetClassNameW FakeClassName
#define SetWindowPos FakePos
#define SetWindowRgn FakeRegion
#define InvalidateRect FakeInvalidate
#define GetWindowTextW FakeText
#define GetForegroundWindow FakeForeground
#define GetAsyncKeyState FakeKey
#define DestroyWindow FakeDestroy
#define GetWindowLongPtrW FakeUserData
#define SetCapture FakeCapture
#define GetCapture FakeCaptured
#define ReleaseCapture FakeRelease
#define OpenIcon FakeTouch
#define SetForegroundWindow FakeTouch
#define BringWindowToTop FakeTouch
#define SetWindowPlacement FakePlacement
#define ShowWindowAsync FakeShowAsync
#define MoveWindow FakeMove
#define PostMessageW FakePost
#define SendMessageW FakeSend
#define Log FakeLog
#include "../src/window_preview.cpp"
#undef Log

AppModel g_apps;
void AppModel::ActivateWindow(HWND) { ++activations; }
static std::vector<HWND> asked;   // the windows the preview asked the AppModel to close
void AppModel::AskToClose(HWND h, const std::wstring&) { asked.push_back(h); }
static void Check(bool value, const char* message) {
    ++checks; if (!value) { std::printf("FAIL: %s\n", message); std::exit(1); }
}
// Each block its own handles (base), so what the preview's log remembers of one block's windows
// does not meet the next block's.
static std::shared_ptr<AppItem> MakeApp(int count, INT_PTR base = 0x1000) {
    auto app = std::make_shared<AppItem>(); app->name = L"Fixture";
    sources.clear(); registeredFor.clear();
    for (int i = 0; i < count; ++i) { HWND h = (HWND)(base + i); sources[h] = {}; app->windows.push_back(h); }
    return app;
}
static bool Has(const std::wstring& line, const wchar_t* part) { return line.find(part) != std::wstring::npos; }
static const wchar_t kMinimizedText[] = L"최소화된 창 · 클릭하여 복원";
int main() {
    for (RECT mon : {RECT{0,0,3840,2160}, RECT{567,2160,3127,3760}, RECT{-1280,-800,0,0}, RECT{0,0,800,600}})
    for (Edge edge : {Edge::Left, Edge::Right, Edge::Top, Edge::Bottom})
    for (int count : {1,2,8,40}) {
        auto app = MakeApp(count);
        WindowPreview p; p.hwnd = (HWND)0x8080; p.app = app; p.monitor = mon; p.edge = edge; p.scale = 1;
        int cx = (mon.left+mon.right)/2, cy = (mon.top+mon.bottom)/2;
        switch(edge) {
        case Edge::Left: p.anchor={mon.left,cy-24,mon.left+100,cy+24}; break;
        case Edge::Right: p.anchor={mon.right-100,cy-24,mon.right,cy+24}; break;
        case Edge::Top: p.anchor={cx-24,mon.top,cx+24,mon.top+100}; break;
        case Edge::Bottom: p.anchor={cx-24,mon.bottom-100,cx+24,mon.bottom}; break;
        }
        p.Refresh(); shown=true;
        std::set<HWND> reached;
        for (int page=0; page<count; ++page) {
            Check(p.bounds.left>=mon.left && p.bounds.right<=mon.right && p.bounds.top>=mon.top && p.bounds.bottom<=mon.bottom,"popup stays on selected monitor");
            Check(edge==Edge::Left ? p.bounds.left>p.anchor.right : edge==Edge::Right ? p.bounds.right<p.anchor.left : edge==Edge::Top ? p.bounds.top>p.anchor.bottom : p.bounds.bottom<p.anchor.top,"popup clears dock on all four edges");
            for(int i=p.first;i<std::min(p.first+p.capacity,count);++i) {
                auto& e=p.entries[i]; reached.insert(e.source);
                POINT center{(e.card.left+e.card.right)/2,(e.card.top+e.card.bottom)/2};
                Check(p.Hit(center)==i,"each displayed card selects its own source");
                Check(e.thumbnail!=nullptr,"visible ordinary window gets DWM relationship");
            }
            Check(thumbnails.size()==(size_t)std::min(p.capacity,count-p.first),"only current page retains thumbnails");
            int before=p.first;p.ChangePage(1);if(before==p.first)break;
        }
        Check((int)reached.size()==count,"all windows remain reachable through pages");
        p.Dismiss();Check(thumbnails.empty()&&!shown,"dismiss releases all DWM handles and hides popup");
    }
    Check(logs.empty(),"no dock.log lines for windows that are not minimized");
    {
        auto app=MakeApp(3);sources[app->windows[0]].minimized=true;sources[app->windows[0]].dwm={160,28};sources[app->windows[1]].protectedContent=true;
        WindowPreview p;p.hwnd=(HWND)0x8080;p.app=app;p.monitor={0,0,1920,1080};p.anchor={900,980,950,1080};
        p.Refresh();
        Check(!p.entries[0].thumbnail&&p.entries[0].fallback==kMinimizedText,"minimized window without a kept image stays minimized with selectable fallback");
        Check(!p.entries[1].thumbnail&&p.entries[1].fallback==L"미리보기가 제한된 창","known capture restriction does not request thumbnail");
        Check(!registeredFor.count(app->windows[1]),"a protected window's thumbnail is never requested");
        Check(p.entries[2].thumbnail!=nullptr,"ordinary sibling still has preview");
        sources[app->windows[2]].pid=9;Check(!p.Valid(p.entries[2]),"recycled handle from another process cannot be activated");
        sources[app->windows[2]].alive=false;p.Refresh();Check(p.entries.size()==2,"closed window is removed");
        failDwm=true;sources[app->windows[0]].minimized=false;p.Refresh();
        Check(!p.entries[0].thumbnail&&p.entries[0].fallback==L"미리보기 없음 · 클릭하여 열기","DWM failure keeps a selectable title");
        failDwm=false;
        p.Dismiss();
    }
    {   // Minimized windows show the image DWM kept of them, as on the taskbar; the placeholder only without one.
        auto app=MakeApp(12,0x2000);
        auto src=[&](int i)->Source&{return sources[app->windows[i]];};
        for(int i=0;i<12;++i)src(i).minimized=true;
        src(0).dwm={1904,1032};                 // minimized after it was on screen
        src(1).dwm={160,28};                    // DWM answering with no more than its caption
        src(2).dwm={0,0};
        src(3).dwmFails=true;                   // DWM did not answer: asked again, like a live window
        src(4).dwm={480,84};src(4).minimizedRect={-32000,-32000,-31520,-31916};   // 300 %: its caption, as big as itself
        src(5).dwm={2896,1720};                 // maximized before it was minimized
        src(6).protectedContent=true;
        src(7).dwm={160,28};src(7).minimizedRect={-32000,-32000,-31760,-31958};   // a caption, though not its own size (DPI)
        src(8).dwm={476,80};src(8).minimizedRect={-32000,-32000,-31520,-31916};   // a caption a few px short of its own size
        src(9).dwm={486,90};src(9).minimizedRect={-32000,-32000,-31520,-31916};   // ...or a few px over it
        src(10).dwm={1904,1032};src(10).minimizedRect={-32000,-32000,-31600,-31930};   // 250 %: a real image, caption 400x70
        src(11).updateFails=true;               // an image DWM then did not show
        WindowPreview p;p.hwnd=(HWND)0x8080;p.app=app;p.monitor={0,0,3840,2160};p.anchor={1900,2060,1950,2160};p.edge=Edge::Bottom;p.scale=1;
        logs.clear();sourceTouched=0;
        p.Refresh();
        Check(p.capacity>=12&&p.entries.size()==12,"fixture: all twelve cards on one page");
        for(int i:{0,5,10}){
            auto& e=p.entries[i];
            Check(e.thumbnail&&e.fallback.empty(),"a minimized window shows the image DWM kept of it");
            Check(lastProps.count(e.thumbnail)&&lastProps[e.thumbnail].fVisible&&lastProps[e.thumbnail].opacity==255,"its kept image is shown, opaque");
            RECT d=lastProps[e.thumbnail].rcDestination;SIZE s=src(i).dwm;
            Check(d.left>=e.image.left&&d.right<=e.image.right&&d.top>=e.image.top&&d.bottom<=e.image.bottom,"its kept image fits its card");
            double want=double(s.cx)/s.cy,got=double(d.right-d.left)/(d.bottom-d.top);
            Check(std::fabs(got-want)/want<0.03,"its kept image keeps its shape");
        }
        for(int i:{1,2,3,4,6,7,8,9,11})Check(!p.entries[i].thumbnail&&p.entries[i].fallback==kMinimizedText,"a minimized window without a kept image (or a protected one) shows the placeholder");
        Check(thumbnails.size()==3,"only the three kept images hold a DWM relationship");
        Check(!registeredFor.count(app->windows[6]),"a protected minimized window's thumbnail is never requested");
        Check(logs.size()==11,"one dock.log line per minimized window that was asked about");
        int shownLines=0,placeholders=0,noAnswer=0,notShown=0;
        for(auto& l:logs){shownLines+=Has(l,L": shown");placeholders+=Has(l,L": placeholder");noAnswer+=Has(l,L"DWM gave no image");notShown+=Has(l,L"but did not show it");
            Check(Has(l,L"preview: minimized window of Fixture (FixtureClass)")&&!Has(l,L"Fixture window"),"lines name the app and window class, not the title");}
        Check(shownLines==3&&placeholders==8&&noAnswer==1&&notShown==1,"lines say what DWM reported and what the card shows");
        Check(std::any_of(logs.begin(),logs.end(),[](const std::wstring& l){return Has(l,L"DWM reports 160x28, the window is 160x28");}),"a caption-only answer is logged with the window's own size");
        int regs=registrations;size_t lines=logs.size();
        p.Refresh();p.Refresh();
        Check(registeredFor[app->windows[3]]==3&&registeredFor[app->windows[11]]==3&&registrations==regs+4,"a window DWM did not answer for, or did not show, is asked again on every refresh, like a live one");
        for(int i:{0,1,2,4,5,7,8,9,10})Check(registeredFor[app->windows[i]]==1,"a kept image is not registered anew, and a minimized window without an image is not asked again while it stays minimized");
        Check(logs.size()==lines,"no line again for the same outcome");
        // DWM lost the kept image (DWM restarted): the placeholder, and no more asking
        src(0).dwm={160,28};p.Refresh();
        Check(!p.entries[0].thumbnail&&p.entries[0].fallback==kMinimizedText&&logs.size()==lines+1,"a kept image that is gone falls back to the placeholder, logged once");
        p.Refresh();Check(registeredFor[app->windows[0]]==1,"...and is not asked about again");
        // restored: the live preview again; minimized once more after being on screen: the image DWM kept
        src(1).minimized=false;src(1).dwm={1280,720};p.Refresh();
        Check(p.entries[1].thumbnail&&p.entries[1].fallback.empty(),"a restored window gets its live preview");
        HTHUMBNAIL live=p.entries[1].thumbnail;int before=registeredFor[app->windows[1]];
        src(1).minimized=true;p.Refresh();
        Check(p.entries[1].thumbnail==live&&p.entries[1].fallback.empty()&&registeredFor[app->windows[1]]==before,"minimized while the preview is open: the same relationship shows the kept image");
        // the next opening asks again (a new popup, new entries)
        p.Dismiss();Check(thumbnails.empty(),"dismiss releases the kept images' relationships too");
        p.app=app;shown=true;p.Refresh();
        Check(registeredFor[app->windows[2]]==2&&!p.entries[2].thumbnail,"the next opening asks DWM once again");
        Check(sourceTouched==0,"no window is restored, shown, moved, activated or sent a command for its preview");
        p.Dismiss();
    }
    {   // A window shown again while its card is on another page is asked anew when its page comes back.
        auto app=MakeApp(6,0x3000);
        auto& first=sources[app->windows[0]];first.minimized=true;first.dwm={160,28};   // no image (a caption-size answer)
        WindowPreview p;p.hwnd=(HWND)0x8080;p.app=app;p.monitor={0,0,800,600};p.anchor={375,500,425,600};p.edge=Edge::Bottom;p.scale=1;
        p.Refresh();shown=true;
        Check(p.capacity<6&&p.first==0&&p.entries[0].fallback==kMinimizedText,"fixture: the placeholder on the first of several pages");
        p.ChangePage(1);Check(p.first>0,"fixture: another page");
        first.minimized=false;p.Refresh();        // restored while its card is off the page...
        first.minimized=true;first.dwm={1280,720};p.Refresh();   // ...and minimized again, now with an image
        p.ChangePage(-1);
        Check(p.first==0&&p.entries[0].thumbnail&&p.entries[0].fallback.empty(),"back on its page it shows the image DWM kept");
        Check(sourceTouched==0,"...still without touching it");
        p.Dismiss();
    }
    {   // Without the window's own size, only the size floor tells a caption from an image.
        auto app=MakeApp(2,0x4000);
        for(HWND h:app->windows){sources[h].minimized=true;sources[h].rectFails=true;}
        sources[app->windows[0]].dwm={160,28};sources[app->windows[1]].dwm={1904,1032};
        WindowPreview p;p.hwnd=(HWND)0x8080;p.app=app;p.monitor={0,0,1920,1080};p.anchor={900,980,950,1080};
        p.Refresh();
        Check(!p.entries[0].thumbnail&&p.entries[0].fallback==kMinimizedText,"a caption-sized answer is no image, own size or not");
        Check(p.entries[1].thumbnail&&p.entries[1].fallback.empty(),"an image of a window's size is shown");
        p.Dismiss();
    }
    {
        auto app=MakeApp(1);WindowPreview p;p.hwnd=(HWND)0x8080;
        RECT anchor{0,400,100,450},mon{0,0,1920,1080};POINT cursor{50,425};
        auto tick=[&](std::shared_ptr<AppItem> hovered,POINT pos,double time,bool allowed=true){p.Update(nullptr,hovered,anchor,mon,Edge::Left,1,pos,time,allowed);};
        tick(app,cursor,0);tick(app,cursor,0.44);Check(!p.Open(),"no popup before dwell threshold");
        tick(app,cursor,0.46);Check(p.Open(),"popup opens after dwell without activation");
        POINT bridge{(p.bridge.left+p.bridge.right)/2,(p.bridge.top+p.bridge.bottom)/2};
        tick(nullptr,bridge,0.5);Check(p.Open()&&p.Contains(bridge),"crossing gap keeps popup open");
        tick(nullptr,{1800,900},0.6);tick(nullptr,{1800,900},0.9);Check(!p.Open(),"leaving closes after grace period");
        tick(app,cursor,1);tick(app,cursor,1.5);escapeState=(SHORT)0x8000;tick(app,cursor,1.6);escapeState=0;
        tick(app,cursor,3);Check(!p.Open(),"escape does not immediately reopen under stationary cursor");
        tick(nullptr,{1800,900},3.1);tick(app,cursor,3.2);tick(app,cursor,3.7);Check(p.Open(),"leaving and returning permits reopening");
        foreground=(HWND)0x9999;tick(app,cursor,3.8);Check(!p.Open(),"foreground switch dismisses popup");
        tick(nullptr,{1800,900},4);tick(app,cursor,4.1);tick(app,cursor,4.6);tick(app,cursor,4.7,false);
        Check(!p.Open(),"fullscreen or drag inhibition closes popup");
    }
    {
        auto app=MakeApp(2);WindowPreview p;p.hwnd=(HWND)0x8080;p.app=app;p.monitor={0,0,1920,1080};p.anchor={900,980,950,1080};
        sources[app->windows[0]].minimized=true;   // a minimized card restores its window like any other
        p.Refresh();shown=true;current=&p;
        RECT card=p.entries[0].card;LPARAM point=MAKELPARAM((card.left+card.right)/2,(card.top+card.bottom)/2);
        WindowPreview::Proc(p.hwnd,WM_LBUTTONDOWN,0,point);
        sources[p.entries[0].source].pid=77;
        WindowPreview::Proc(p.hwnd,WM_LBUTTONUP,0,point);
        Check(activations==0,"a changed source between press and release cannot be activated");
        sources[p.entries[0].source].pid=p.entries[0].pid;
        WindowPreview::Proc(p.hwnd,WM_LBUTTONDOWN,0,point);
        WindowPreview::Proc(p.hwnd,WM_LBUTTONUP,0,point);
        Check(activations==1&&!p.Open(),"matching press and release routes activation once and dismisses");
        current=nullptr;
    }
    {   // Closing from a card, as on the taskbar's previews: its close button, or a middle click on it.
        auto app=MakeApp(4,0x5000);
        WindowPreview p;p.hwnd=(HWND)0x8080;p.app=app;p.monitor={0,0,1920,1080};p.anchor={900,980,950,1080};p.edge=Edge::Bottom;p.scale=1;
        p.Refresh();shown=true;current=&p;sourceTouched=0;asked.clear();p.foreground=foreground;
        const int acts=activations;
        auto center=[](RECT r){return MAKELPARAM((r.left+r.right)/2,(r.top+r.bottom)/2);};
        auto click=[&](UINT down,LPARAM at,LPARAM to){WindowPreview::Proc(p.hwnd,down,0,at);WindowPreview::Proc(p.hwnd,down+1,0,to);};
        for(int i=0;i<4;++i){
            RECT x=p.CloseRect(p.entries[i]),in{};POINT c{(x.left+x.right)/2,(x.top+x.bottom)/2};
            Check(p.HitClose(c)==i&&p.Hit(c)==i,"each card's close button is its own, on its card");
            Check(x.right<=p.entries[i].card.right&&x.top>=p.entries[i].card.top&&!IntersectRect(&in,&x,&p.entries[i].image),"...in its title row, clear of the picture");
            POINT pic{(p.entries[i].image.left+p.entries[i].image.right)/2,(p.entries[i].image.top+p.entries[i].image.bottom)/2};
            Check(p.HitClose(pic)==-1,"the picture is no close button");
        }
        const LPARAM x0=center(p.CloseRect(p.entries[0])),x1=center(p.CloseRect(p.entries[1])),b1=center(p.entries[1].image),
                     b2=center(p.entries[2].image),b3=center(p.entries[3].image);
        click(WM_LBUTTONDOWN,x0,x0);
        Check(asked.size()==1&&asked[0]==app->windows[0],"the close button asks its window to close");
        Check(activations==acts&&p.Open(),"...and nothing else: no activation, the popup stays");
        Check(p.entries[0].closing&&lastProps[p.entries[0].thumbnail].opacity==kClosingOpacity,"a window asked to close is dimmed");
        click(WM_LBUTTONDOWN,x0,x0);
        Check(asked.size()==1&&activations==acts,"pressed again while it goes: nothing more is asked and nothing activated");
        click(WM_LBUTTONDOWN,x1,b1);click(WM_LBUTTONDOWN,b1,x1);
        Check(asked.size()==1&&activations==acts,"a press and a release on different things do nothing");
        WindowPreview::Proc(p.hwnd,WM_LBUTTONDOWN,0,x1);sources[app->windows[1]].pid=77;WindowPreview::Proc(p.hwnd,WM_LBUTTONUP,0,x1);
        Check(asked.size()==1,"a handle another program got between press and release is not asked to close");
        sources[app->windows[1]].pid=3;
        click(WM_MBUTTONDOWN,b2,b3);
        Check(asked.size()==1,"a middle click let go on another card does nothing");
        click(WM_MBUTTONDOWN,b2,b2);
        Check(asked.size()==2&&asked[1]==app->windows[2]&&activations==acts&&p.Open(),"a middle click on a card asks its window to close");
        // After a close, a new front window is the close's doing (the next window, or the program's question).
        RECT anchor=p.anchor;POINT on{(p.bounds.left+p.bounds.right)/2,(p.bounds.top+p.bounds.bottom)/2};
        auto tick=[&](double t){p.Update(nullptr,app,anchor,p.monitor,Edge::Bottom,1,on,t,true);};
        tick(10);foreground=(HWND)0x7777;tick(10.5);
        Check(p.Open()&&p.foreground==(HWND)0x7777,"a new front window right after a close keeps the popup");
        sources[app->windows[0]].alive=false;tick(10.8);
        Check(p.Open()&&p.entries.size()==3&&p.entries[0].source==app->windows[1],"a closed window's card goes, the rest stay");
        Check(p.entries[1].closing,"one still going stays dimmed...");
        tick(12.1);
        Check(!p.entries[1].closing&&lastProps[p.entries[1].thumbnail].opacity==255,"...until kCloseAgain: then whole again (it asked something, or refused)");
        click(WM_LBUTTONDOWN,center(p.CloseRect(p.entries[1])),center(p.CloseRect(p.entries[1])));
        Check(asked.size()==3&&asked[2]==app->windows[2],"...and its close button works again");
        tick(12.2);foreground=(HWND)0x7778;tick(13.9);
        Check(!p.Open(),"a front window changing long after the last close closes the popup, as before");
        // the last window closed: the popup goes with it
        auto one=MakeApp(1,0x6000);WindowPreview q;q.hwnd=(HWND)0x8080;q.app=one;q.monitor={0,0,1920,1080};q.anchor={900,980,950,1080};
        q.Refresh();shown=true;current=&q;q.foreground=foreground;asked.clear();
        WindowPreview::Proc(q.hwnd,WM_LBUTTONDOWN,0,center(q.CloseRect(q.entries[0])));WindowPreview::Proc(q.hwnd,WM_LBUTTONUP,0,center(q.CloseRect(q.entries[0])));
        Check(asked.size()==1&&q.Open(),"the only window asked to close: the popup waits for it");
        sources[one->windows[0]].alive=false;
        q.Update(nullptr,one,q.anchor,q.monitor,Edge::Bottom,1,on,20,true);
        Check(!q.Open()&&thumbnails.empty(),"...and goes with it");
        Check(sourceTouched==0&&activations==acts,"closing from the preview neither touches a window itself nor activates one");
        current=nullptr;
    }
    {   // The handle went to another program between press and release, and its card came back for that one.
        auto app=MakeApp(2,0x7000);
        WindowPreview p;p.hwnd=(HWND)0x8080;p.app=app;p.monitor={0,0,1920,1080};p.anchor={900,980,950,1080};
        p.Refresh();shown=true;current=&p;asked.clear();
        auto center=[](RECT r){return MAKELPARAM((r.left+r.right)/2,(r.top+r.bottom)/2);};
        WindowPreview::Proc(p.hwnd,WM_LBUTTONDOWN,0,center(p.CloseRect(p.entries[0])));
        sources[app->windows[0]].pid=77;p.Refresh();
        Check(p.entries.size()==2&&p.entries[1].source==app->windows[0]&&p.entries[1].pid==77,"fixture: its card rebuilt for the other program");
        WindowPreview::Proc(p.hwnd,WM_LBUTTONUP,0,center(p.CloseRect(p.entries[1])));
        Check(asked.empty(),"...is not asked to close by the button pressed for the first one");
        WindowPreview::Proc(p.hwnd,WM_MBUTTONDOWN,0,center(p.entries[0].image));
        sources[app->windows[1]].pid=78;p.Refresh();
        WindowPreview::Proc(p.hwnd,WM_MBUTTONUP,0,center(p.entries[1].image));
        Check(asked.empty(),"...nor by a middle click begun on the first one");
        p.Dismiss();current=nullptr;
    }
    Check(thumbnails.empty()&&activations==1,"fixtures route to fake activation and release all thumbnails");
    std::printf("PASS: %d preview checks, %d thumbnail updates\n",checks,updates);
}
