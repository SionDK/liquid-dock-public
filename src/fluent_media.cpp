#include "fluent_internal.h"
#ifndef LD_FLUENT_NO_MEDIA
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#endif

namespace fluentflyout {

// ---------------------------------------------------------------- media sessions
// FluentFlyout counts as "playing" any media session (Windows' media transport controls) its app
// filter lets through; "hide completely" hides the widget when there is none. Only then does the slot
// follow the sessions (WidgetState): otherwise the list is not kept and the media thread sleeps (WantMedia).
std::atomic<bool> g_mediaWanted{ false };    // the UI thread uses the list (WantMedia)
std::atomic<bool> g_namesWanted{ false };   // found, its app filter on: the media sessions' apps are named (PublishSessions)
fluentlogic::Steady g_media;                 // UI thread: g_rawMedia without the flicker of sessions being replaced

namespace {

std::mutex g_mediaMu;
std::vector<std::wstring> g_sessions;        // source app ids
std::unordered_map<std::wstring, std::wstring> g_appNames;   // FluentFlyout's names for their apps (while its app filter is on)
std::atomic<int> g_mediaVersion{ 0 };
std::atomic<bool> g_mediaKnown{ false };
std::mutex g_publishMu;                      // the media thread and Windows' callbacks: one list at a time
HANDLE g_mediaThread = nullptr, g_mediaStop = nullptr;
HANDLE g_mediaWake = nullptr;                // g_mediaWanted changed: the media thread looks at once
bool g_rawMedia = true;                      // UI thread: something plays (by the sessions as last listed)
struct { int sessions = -1, settings = -1; } g_rawMediaFor;   // the session and settings versions g_rawMedia was worked out for
bool g_listSeen = false;                     // UI thread: g_mediaKnown when the sessions were last looked at
double g_mediaWantedAt = -1;                 // UI thread: when the list was last wanted again (WantMedia)

using fluentsettings::Trimmed;

std::wstring FileDescription(const std::wstring& path) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return {};
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return {};
    struct LangCp { WORD lang, cp; }* tr = nullptr;
    UINT trLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &trLen) || trLen < sizeof(LangCp)) return {};
    wchar_t q[128];
    swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
    wchar_t* val = nullptr;
    UINT len = 0;
    return VerQueryValueW(buf.data(), q, (void**)&val, &len) && len > 1 ? std::wstring(val) : std::wstring();
}

// FluentFlyout compares names and ids as .NET's OrdinalIgnoreCase does: the system's uppercase table.
bool EqualIgnoringCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool ContainsIgnoringCase(const std::wstring& s, const std::wstring& part) {
    return part.empty() || FindStringOrdinal(FIND_FROMSTART, s.c_str(), (int)s.size(), part.c_str(), (int)part.size(), TRUE) >= 0;
}

// String.Replace(what, "", OrdinalIgnoreCase): every occurrence, left to right, in one pass.
std::wstring WithoutIgnoringCase(const std::wstring& s, const wchar_t* what) {
    const int n = (int)wcslen(what);
    std::wstring out;
    for (size_t at = 0;;) {
        int p = at < s.size() ? FindStringOrdinal(FIND_FROMSTART, s.c_str() + at, (int)(s.size() - at), what, n, TRUE) : -1;
        if (p < 0) return out.append(s, at, std::wstring::npos);
        out.append(s, at, p);
        at += p + n;
    }
}

// What FluentFlyout looks for in the running programs to name a media app: the parts of its session id
// (split at the dots, "com", "github" and "exe" taken out), the id itself, and "msedge" for Edge's ids.
std::vector<std::wstring> NameVariants(const std::wstring& id) {
    std::vector<std::wstring> variants;
    for (size_t start = 0;;) {
        size_t dot = id.find(L'.', start);
        std::wstring part = id.substr(start, dot == std::wstring::npos ? std::wstring::npos : dot - start);
        for (const wchar_t* junk : { L"com", L"github", L"exe" }) part = WithoutIgnoringCase(part, junk);
        part = Trimmed(part);
        if (!part.empty()) variants.push_back(part);
        if (dot == std::wstring::npos) break;
        start = dot + 1;
    }
    variants.push_back(id);
    if (std::any_of(variants.begin(), variants.end(), [](const std::wstring& v) {
            return ContainsIgnoringCase(v, L"MicrosoftEdge") || EqualIgnoringCase(v, L"MSEdge") ||
                   (v.size() >= 7 && EqualIgnoringCase(v.substr(v.size() - 7), L"!MSEdge"));
        }))
        variants.push_back(L"msedge");
    return variants;
}

// A running program as FluentFlyout sees it (.NET's Process): its name (the file name without ".exe"),
// its path (empty when it cannot be read as FluentFlyout reads it - those are skipped) and its main
// window (as .NET's MainWindowHandle: its first visible top-level window without an owner).
struct Program {
    std::wstring name, path;
    HWND window = nullptr;
};

bool IsVariant(const std::vector<std::wstring>& variants, const std::wstring& name) {
    return std::any_of(variants.begin(), variants.end(), [&](const std::wstring& v) { return EqualIgnoringCase(v, name); });
}

// FluentFlyout's pick: the first program named like one of the variants (a window or not), else the
// first one with a main window whose path contains one; -1 when none.
int PickProgram(const std::vector<std::wstring>& variants, const std::vector<Program>& programs) {
    int byPath = -1;
    for (int i = 0; i < (int)programs.size(); ++i) {
        const Program& p = programs[i];
        if (p.path.empty()) continue;
        if (IsVariant(variants, p.name)) return i;
        if (byPath < 0 && p.window &&
            std::any_of(variants.begin(), variants.end(), [&](const std::wstring& v) { return ContainsIgnoringCase(p.path, v); }))
            byPath = i;
    }
    return byPath;
}

// The programs FluentFlyout would look at for these variants: those with a main window, and those
// named like one of them.
std::vector<Program> RunningPrograms(const std::vector<std::wstring>& variants) {
    std::unordered_map<DWORD, HWND> windows;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            ((std::unordered_map<DWORD, HWND>*)lp)->try_emplace(pid, h);   // the first in z-order
        }
        return TRUE;
    }, (LPARAM)&windows);
    std::vector<Program> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        Program p;
        p.name = pe.szExeFile;
        if (p.name.size() > 4 && EqualIgnoringCase(p.name.substr(p.name.size() - 4), L".exe")) p.name.resize(p.name.size() - 4);
        if (auto w = windows.find(pe.th32ProcessID); w != windows.end()) p.window = w->second;
        if (!p.window && !IsVariant(variants, p.name)) continue;
        // FluentFlyout reads its main module (query and read access): one it cannot open so is skipped
        if (HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID)) {
            p.path = ImagePath(h);
            CloseHandle(h);
        }
        if (!p.path.empty()) out.push_back(std::move(p));
    }
    CloseHandle(snap);
    return out;
}

// The shell's name for an app id, as FluentFlyout asks for it (shell:AppsFolder, ParseName(id).Name):
// packaged apps, and programs whose id names no running program (Firefox's is a hash).
std::wstring ShellAppName(const std::wstring& id) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // the media thread has it already
    std::wstring name;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, id.c_str(), IID_PPV_ARGS(&item)))) {
        PWSTR n = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVE, &n)) && n) name = n;
        CoTaskMemFree(n);
        item->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return name;
}

// FluentFlyout's name for a media app (MediaPlayerData.GetAndCacheMediaPlayerData; its app filter and
// its own filter lists use it): the program it picks - that one's description, else its main window's
// title; with none, the shell's name; else the id. Not on the UI thread: it looks at every program
// with a window, and the shell's name comes through COM (PublishSessions).
std::wstring AppName(const std::wstring& id) {
    const std::vector<std::wstring> variants = NameVariants(id);
    const std::vector<Program> programs = RunningPrograms(variants);
    if (int i = PickProgram(variants, programs); i >= 0) {
        std::wstring name = FileDescription(programs[i].path);
        if (Trimmed(name).empty() && programs[i].window) {
            wchar_t title[256] = {};
            GetWindowTextW(programs[i].window, title, ARRAYSIZE(title));   // another program's: no message sent
            name = title;
        }
        return Trimmed(name).empty() ? id : name;
    }
    std::wstring shell = ShellAppName(id);
    return Trimmed(shell).empty() ? id : shell;
}

// A new session list for the UI thread (the media thread, Windows' callbacks: one at a time, under
// g_publishMu). While FluentFlyout's app filter is on, the apps are named first, outside the lock: the
// new ones, and - when the list changed - those named by their id only (FluentFlyout tries those again
// too). A name once found is kept, as FluentFlyout keeps it.
void PublishSessions(std::vector<std::wstring> ids) {
    std::vector<std::wstring> unnamed;
    {
        std::lock_guard<std::mutex> lk(g_mediaMu);
        const bool changed = !g_mediaKnown || ids != g_sessions;
        if (g_namesWanted)
            for (auto& id : ids) {
                auto it = g_appNames.find(id);
                if ((it == g_appNames.end() || (changed && it->second == id)) && std::find(unnamed.begin(), unnamed.end(), id) == unnamed.end())
                    unnamed.push_back(id);
            }
        if (!changed && unnamed.empty()) return;
    }
    std::vector<std::wstring> names;
    for (auto& id : unnamed) names.push_back(AppName(id));
    std::vector<bool> renamed(unnamed.size());
    {
        std::lock_guard<std::mutex> lk(g_mediaMu);
        if (g_appNames.size() > 64)
            std::erase_if(g_appNames, [&](const auto& e) { return std::find(ids.begin(), ids.end(), e.first) == ids.end(); });
        for (size_t i = 0; i < unnamed.size(); ++i) {
            auto& known = g_appNames[unnamed[i]];
            renamed[i] = known != names[i];
            known = names[i];
        }
        if (g_mediaWanted) {   // not kept when the UI stopped using the list meanwhile: it would be old when wanted again
            g_sessions = std::move(ids);
            g_mediaKnown = true;
            ++g_mediaVersion;
        }
    }
    for (size_t i = 0; i < unnamed.size(); ++i)
        if (renamed[i]) Log(L"fluentflyout: media app %s - FluentFlyout's name for it \"%s\" (its app filter)", unnamed[i].c_str(), names[i].c_str());
}

// One look at the sessions (the media thread's, Windows' change callbacks); `list` reads them. Nothing
// is asked while the UI thread does not use the list.
template <class List>
void Refresh(const List& list) {
    std::lock_guard<std::mutex> one(g_publishMu);   // a list read earlier is never published after a newer one
    if (g_mediaWanted) PublishSessions(list());
}

#ifndef LD_FLUENT_MEDIA_CHECK_MS
#define LD_FLUENT_MEDIA_CHECK_MS 5000   // tests look more often
#endif

// The media thread after its first look: while the list is wanted, a look every 5 s (a change Windows
// did not report: a player that crashed) and one at once when it is wanted again; while it is not, no
// wake-ups at all. Returns when the dock exits.
template <class Check>
void MediaRounds(const Check& check) {
    const HANDLE events[2] = { g_mediaStop, g_mediaWake };
    for (;;) {
        const DWORD r = WaitForMultipleObjects(2, events, FALSE, g_mediaWanted ? LD_FLUENT_MEDIA_CHECK_MS : INFINITE);
        if (r != WAIT_TIMEOUT && r != WAIT_OBJECT_0 + 1) return;
        check();
    }
}

#ifndef LD_FLUENT_NO_MEDIA
DWORD WINAPI MediaThread(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        using namespace winrt::Windows::Media::Control;
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto refresh = [manager] {
            Refresh([&] {
                std::vector<std::wstring> ids;
                for (auto const& session : manager.GetSessions()) ids.emplace_back(session.SourceAppUserModelId().c_str());
                return ids;
            });
        };
        refresh();
        auto token = manager.SessionsChanged([refresh](auto&&, auto&&) { try { refresh(); } catch (...) {} });
        MediaRounds([&] { try { refresh(); } catch (...) {} });
        manager.SessionsChanged(token);
    } catch (...) {
        Log(L"fluentflyout: media sessions unavailable - the widget's slot stays open");
    }
    winrt::uninit_apartment();
    return 0;
}
#endif

bool MediaEvents() {
    if (!g_mediaStop) g_mediaStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_mediaWake) g_mediaWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g_mediaStop && g_mediaWake;
}

// Made the first time the list is wanted: with "hide completely" off FluentFlyout's widget never needs it.
void StartMedia() {
#ifndef LD_FLUENT_NO_MEDIA
    if (g_mediaThread || !MediaEvents()) return;
    ResetEvent(g_mediaStop);
    g_mediaThread = CreateThread(nullptr, 0, MediaThread, nullptr, 0, nullptr);
#endif
}

}   // namespace

// When the dock exits (Shutdown): the media thread is told to stop and waited for a moment.
void StopMedia() {
    if (g_mediaThread) {
        SetEvent(g_mediaStop);
        WaitForSingleObject(g_mediaThread, 1500);   // a media service that hangs does not hold up exit
        CloseHandle(g_mediaThread);
        g_mediaThread = nullptr;
    }
}

// Whether the UI thread uses the session list (Tick). When it stops, the list is dropped: wanted again,
// the next list is a first one again (UpdateHasMedia). What counted last (g_media) is kept until then.
void WantMedia(bool on) {
    if (on == g_mediaWanted) return;
    g_mediaWanted = on;
    if (on) {
        g_mediaWantedAt = NowSeconds();
        StartMedia();
    } else {
        {
            std::lock_guard<std::mutex> lk(g_mediaMu);
            g_mediaKnown = false;
        }
        g_rawMedia = true;
        g_rawMediaFor.sessions = g_rawMediaFor.settings = -1;
        g_listSeen = false;
    }
    if (g_mediaWake) SetEvent(g_mediaWake);   // the media thread looks at once, or goes to sleep
}

// Wanted again and its first list not in yet: the media thread looks at once (Windows answers within
// milliseconds), so the slot stays as it is for that moment (WidgetState) instead of opening on "not
// known". At most a second: a media service that does not answer leaves it not known (counts as playing).
constexpr double kFirstListSeconds = 1.0;
bool FirstListPending(double now) { return g_mediaWanted && !g_listSeen && now - g_mediaWantedAt < kFirstListSeconds; }

namespace {

// FluentFlyout's app filter (MainWindow.IsSessionAllowed): an entry matches the app's name in full or
// any part of its session id, ignoring case; blank entries match nothing, entries are not trimmed.
// `name` is null while the app has not been named yet: an entry may be its name, so it counts (nothing
// hidden that may be shown).
bool SessionAllowed(const std::wstring& id, const std::wstring* name) {
    if (!g_ff.filtering) return true;
    bool maybe = false;
    auto matches = [&](const std::wstring& entry) {
        if (Trimmed(entry).empty()) return false;
        if (ContainsIgnoringCase(id, entry)) return true;
        if (!name) { maybe = true; return false; }
        return EqualIgnoringCase(*name, entry);
    };
    if (g_ff.filterMode == 0) return std::none_of(g_ff.blocked.begin(), g_ff.blocked.end(), matches);
    return std::any_of(g_ff.allowed.begin(), g_ff.allowed.end(), matches) || maybe;
}

}   // namespace

// Every pass: the session list is looked at again only when it or the settings changed; the steady
// state holds "nothing playing" back for a moment (fluentlogic::Steady) - not for the first list,
// which replaced no session: FluentFlyout hides its widget at once then, so does the slot.
void UpdateHasMedia(double now) {
    int mv = g_mediaVersion, sv = g_ffVersion;
    double hold = 1.5;
    if (mv != g_rawMediaFor.sessions || sv != g_rawMediaFor.settings) {
        g_rawMediaFor.sessions = mv; g_rawMediaFor.settings = sv;
        g_rawMedia = true;   // not known (yet): never hide what may be there
        const bool known = g_mediaKnown;
        if (known) {
            std::vector<std::wstring> ids;
            std::vector<std::optional<std::wstring>> names;
            {
                std::lock_guard<std::mutex> lk(g_mediaMu);
                ids = g_sessions;
                if (g_ff.filtering)
                    for (auto& id : ids) {
                        auto it = g_appNames.find(id);
                        names.push_back(it != g_appNames.end() ? std::optional<std::wstring>(it->second) : std::nullopt);
                    }
            }
            g_rawMedia = false;
            for (size_t i = 0; i < ids.size() && !g_rawMedia; ++i)
                g_rawMedia = SessionAllowed(ids[i], i < names.size() && names[i] ? &*names[i] : nullptr);
            if (!g_listSeen) hold = 0;
        }
        g_listSeen = known;
    }
    if (FirstListPending(now)) return;   // what counted last stands until the first list (no lines for "not known")
    if (g_media.Update(g_rawMedia, now, hold)) Log(L"fluentflyout: %s", g_media.value ? L"media playing" : L"nothing playing");
}

}   // namespace fluentflyout
