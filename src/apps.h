#pragma once
#include "common.h"
#include "app_click.h"
#include "taskbar_apps_logic.h"
#include <atomic>

struct AppItem {
    int id = 0;
    std::wstring name;
    std::wstring aumid;     // AppUserModelID (may be empty)
    std::wstring exe;       // lowercase full path (may be empty)
    std::wstring launch;    // what to ShellExecute (lnk / exe / shell:AppsFolder\aumid)
    bool pinned = false;
    std::vector<HWND> windows;     // z-order, topmost first
    ComPtr<ID2D1Bitmap1> icon;
    bool attention = false;        // window flashed and not yet activated
    double lastAttentionBounce = -1e9;
    double lastChoiceLog = -1e9;   // the log said which of its windows a click brought up (once a minute)
    double launchStarted = 0;      // >0 while waiting for a launched app's window
    int launchDock = -1;
    size_t launchWindowCount = 0;
    bool Running() const { return !windows.empty(); }
};

struct AttentionEvent { int id; };

// A minimized window's restore command, sent on a worker thread (app_click.h says why).
struct RestoreJob {
    std::atomic<int> state{ 0 };            // 0 waiting for the program, 1 handled, 2 not delivered
    std::atomic<double> handledAt{ 0 };
    std::atomic<bool> ignored{ false };     // handled, but the window stayed down: the system's restore followed
    std::atomic<unsigned long> error{ 0 };  // why it was not delivered (GetLastError; 1460: no answer in time)
};

class AppModel {
public:
    std::vector<std::shared_ptr<AppItem>> items;   // pinned first (pin order), then running unpinned
    std::vector<AttentionEvent> attentionEvents;    // consumed by the primary dock each frame
    int structureVersion = 0;                       // bumps when items are added/removed/reordered

    void Init();
    void ReloadIcons();                             // after a GPU reset (bitmaps belong to the old device)
    void Tick(double now);                          // periodic refresh
    void MarkDirty() { dirty = true; }
    void OnShellHook(WPARAM code, LPARAM lp);

    // monitor: the clicked dock's (its taskbar's button is pressed first: taskbar_apps.h); nullptr: the pointer's
    void Click(AppItem& a, int dockIndex, HMONITOR monitor = nullptr);
    void Launch(AppItem& a, int dockIndex, bool elevated = false);   // elevated: "run as administrator" (not for store apps)
    void ActivateWindow(HWND h);   // from the window list menu and the previews
    void AskToClose(HWND h, const std::wstring& app);   // one window, from the previews: as its own close button
    void CloseAll(AppItem& a);
    void SetPinned(AppItem& a, bool pinned);
    void MoveTo(AppItem& a, int index, bool pin);
    // index: the dock's app icons left of the drop point. Returns the place it is pinned at (the next file dropped
    // with it goes right after it)
    int  PinFile(const std::wstring& path, int index);
    int  PinnedCount() const;
    std::shared_ptr<AppItem> FindByWindow(HWND h);

private:
    void Refresh();
    void SavePins();
    void LoadPins();
    void ImportTaskbarPins();
    void MigratePins();
    std::shared_ptr<AppItem> NewItem();
    void LoadIconAndName(AppItem& a, HWND sample);

    struct WinCache { std::wstring aumid, family, exe; };   // family: store-app package
    std::unordered_map<HWND, WinCache> winCache;
    // the window being brought up: looked at every appclick::kLook until it came (app_click.h, Next)
    struct Watch {
        HWND window = nullptr, before = nullptr;
        std::wstring name;
        const wchar_t* why = L"";      // the click's decision (appclick::WhyText)
        int windows = 0;               // the app's windows at the click
        double clicked = 0, next = 0;
        int clicks = 1, refocused = 0;
        bool saidBusy = false;
        std::shared_ptr<RestoreJob> restore;   // the restore command sent for it, if it was minimized (or an earlier watch's, unanswered)
        int right = 0;                 // ForegroundRight (system.h) at the last try, for the log
        int rightAtClick = 0;          // ...and at the click
        unsigned long attachError = 0; // AttachThreadInput to the window in front failed with this (0: it did, or was not needed)
        int overAdmin = 0;             // tries handed to the admin helper: an administrator's window was in front (Push)
        // a shell pop-up (Start, Search) in front at the click: closed first, the window brought up once it has
        // gone (app_click.h NextForPopup)
        HWND popup = nullptr;
        int popupClosed = 0;           // times the dock went to send Esc (kPopupTries)
        int popupEsc = 0;              // ...and sent it: not when the pop-up was gone or not shown by then
        double popupClosedAt = 0;
        bool hadPopup = false;         // there was one, for the log
        // a shell window in front at the click that is not shown (cloaked): a closed Start or Search keeping the front -
        // nothing to close, the window brought up as usual; for the log
        bool ghostFront = false;
        // the taskbar's button was pressed for it (taskbar_apps.h): the dock only looks, and steps in only
        // after a press that did nothing while its program answered, or one that restored it behind another
        // window (CheckPress, taskbar_apps_logic.h Judge)
        bool byTaskbar = false;
        bool wantDown = false;         // that press minimizes it
        bool downAtClick = false;      // it was minimized at the click (Seen::downAtClick)
        bool explorerBusy = false;     // the press call took taskbarapps::kExplorerBusy or longer, or may not have arrived (maybe)
        bool frontWasItsProgram = false;   // another window of its program was in front at the click: only this one coming counts
        bool forced = false;           // the dock put it in front itself at a click again: its coming teaches nothing of the press
        double pressedAt = 0;          // when the press was answered (a press that may not have arrived: the click)
        HWND taskbar = nullptr;        // the taskbar its button was pressed on: its thread is asked too (Seen::answers)
        std::shared_ptr<std::atomic<int>> answers;   // its program asked whether it answers (Seen::answers)
    };
    Watch watch;
    // A click handed to the app's button on the taskbar: looked up and pressed on a worker until its result
    // comes in (PollPresses), or the dock does it itself after taskbarapps::kLookupWait without a press under
    // way. Further clicks on that app meanwhile only count.
    struct Press {
        uint32_t job = 0;
        std::shared_ptr<std::atomic<int>> stage;   // the worker's too (taskbar_apps_logic.h Stage)
        int item = 0;                  // AppItem::id
        appclick::Decision d;          // what the click wanted
        int dockIndex = -1, usable = 0, clicks = 1;
        HMONITOR monitor = nullptr;
        double clicked = 0;
        HWND before = nullptr;         // the window in front at the click
        bool frontWasItsProgram = false;   // ...a window of the target's program (Watch::frontWasItsProgram)
        std::wstring aumid, otherAumid, exe;   // what its button was looked for as (taskbarapps::Job), for the log
    };
    std::vector<Press> presses;
    uint32_t pressIds = 0;
    taskbarapps::Learning pressUse;                 // what this run learnt of the taskbar's buttons
    std::unordered_map<int, double> noButtonUntil;  // AppItem::id: no lookup for it before then
    double slowUntil = 0;                           // Explorer was too slow to answer a press: no presses before then
    std::unordered_map<int, int> saidPress;         // AppItem::id: the taskbarapps::Outcome logged for it (+1)
    void ForgetPresses(int id);                     // the app left the dock: its entries above go
    bool saidFirstPress = false;
    // pinned apps started by their taskbar button: does a window of theirs come? (taskbarapps::LearnLaunch)
    struct LaunchCheck { int item = 0; double at = 0; size_t windows = 0; bool maybe = false; };   // maybe: the press may not have arrived
    std::vector<LaunchCheck> launchChecks;
    taskbarapps::LaunchLearning launchUse;
    void ClickWith(AppItem& a, int dockIndex, HMONITOR monitor, bool taskbar);
    bool StartPress(AppItem& a, const appclick::Decision& d, int dockIndex, HMONITOR monitor, int usable, double now);
    void PollPresses(double now);
    taskbarapps::Verdict CheckPress(double now);   // the verdict reached (Judge)
    void CheckLaunches(double now);
    // Presses that may still arrive (taskbarapps::Outcome::Unsure): nothing of the dock's for that window - no
    // click, no preview pick - until the press shows (it went down or came up), or kBusyWait from the click passes.
    struct Maybe { HWND window = nullptr; double until = 0; bool wantDown = false, downAtClick = false; };
    std::vector<Maybe> maybes;
    bool MaybeOnItsWay(HWND h, double now);   // true: a press for it may still arrive
    void ForgetMaybe(HWND h);
    // A window Windows would not put in front for the dock flashes instead (HSHELL_FLASH): that is not
    // its program asking for attention, so no attention bounce for it while the dock brings it up - nor for
    // another window of its program (a browser's window and its apps' are one process, clicked by turns), nor
    // for one tried a moment before another icon was clicked.
    struct QuietFlash { HWND window = nullptr; DWORD pid = 0; double until = 0; };
    std::vector<QuietFlash> quietFlashes;
    void QuietFlashes(HWND h, double now);   // the dock is trying h now
    bool FlashQuiet(HWND h, double now);     // a flash of h is the dock's doing
    struct { HWND window = nullptr; double at = -1e9; } lastUp;   // the last window that came: a click right after never minimizes it
    // The restore commands sent, whichever watch sent them, while their program has not answered (or answered a
    // moment ago, the window not seen up since): a watch ends while its program is busy - the user went on, another
    // icon was clicked - and its command stays on its way. A later click on that window waits for it rather than
    // sends a second.
    std::vector<std::pair<HWND, std::shared_ptr<RestoreJob>>> restores;
    std::shared_ptr<RestoreJob> SendRestore(HWND h);
    std::shared_ptr<RestoreJob> RestoreUnderWay(HWND h, double now);   // its command not answered yet, or just handled
    void ForgetHandledRestores(HWND h);   // it came up: its handled commands are done with
    void BringUp(HWND h, const wchar_t* why, int windows);
    void Push(double now, bool atClick);   // the watched window put in front
    void ClosePopup(double now);           // Esc to the watch's pop-up, while it is in front
    void CheckWatch(double now);
    int nextId = 1;
    bool dirty = true;
    double lastRefresh = 0;
};

extern AppModel g_apps;
// --demo (recording videos): only the pinned apps; other running apps stay off the dock
extern bool g_demoMode;
