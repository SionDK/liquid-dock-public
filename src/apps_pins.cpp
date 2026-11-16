#include "apps.h"
#include "apps_internal.h"
#include "admin_helper.h"
#include "admin_client.h"   // Elevated: the log names a window in front that runs as administrator
#include "app_click.h"
#include "system.h"         // TakeForegroundRight
#include "taskbar_apps.h"   // a click through the app's button on the taskbar
#include "explorer_taskbar.h"   // ExplorerTaskbar: asked whether it answers after a press
#include "gfx.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <fstream>
#include <sstream>
#include <thread>

void AppModel::LoadPins() {
    std::wstring path = AppDataDir() + L"\\pins.txt";
    std::ifstream f(path);
    if (!f) {
        ImportTaskbarPins();
        SavePins();
        g_settings.pinsVersion = 2;
        g_settings.Save();
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        std::wstring w = FromUtf8(line);
        std::vector<std::wstring> parts;
        size_t s = 0, t;
        while ((t = w.find(L'\t', s)) != std::wstring::npos) { parts.push_back(w.substr(s, t - s)); s = t + 1; }
        parts.push_back(w.substr(s));
        if (parts.size() < 4) continue;
        auto a = NewItem();
        a->pinned = true;
        a->name = parts[0]; a->aumid = parts[1]; a->exe = ToLower(parts[2]); a->launch = parts[3];
        // a name earlier versions made of the lowercased path (no file description): named again, as spelled
        if (!a->exe.empty() && !EndsWithI(a->launch, L".lnk") && a->name == FileStem(a->exe)) a->name.clear();
        LoadIconAndName(*a, nullptr);
        items.push_back(a);
    }
    structureVersion++;
}

// One line a pin, its fields between tabs: a tab or line break in a field (a name from a file description or a
// window title) becomes a space. Written whole or not at all (common.h SaveFileWhole), so a crash, a power loss or
// a full disk leaves the last pins.txt whole - an empty one would be taken for the user's choice (LoadPins imports
// nothing then). Flushing waits for the disk a moment, at a pin change only.
void AppModel::SavePins() {
    auto field = [](std::wstring s) { for (auto& c : s) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' '; return s; };
    std::string text;
    for (auto& a : items)
        if (a->pinned) text += ToUtf8(field(a->name) + L"\t" + field(a->aumid) + L"\t" + field(a->exe) + L"\t" + field(a->launch)) + "\r\n";
    SaveFileWhole(AppDataDir() + L"\\pins.txt", text, L"pins");
}

static bool ReadShortcut(const std::wstring& lnk, std::wstring& exe, std::wstring& aumid) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    ComPtr<IPersistFile> pf; sl.As(&pf);
    if (FAILED(pf->Load(lnk.c_str(), STGM_READ))) return false;
    wchar_t target[MAX_PATH * 2] = {};
    if (SUCCEEDED(sl->GetPath(target, ARRAYSIZE(target), nullptr, 0)) && target[0]) {
        wchar_t expanded[MAX_PATH * 2];
        ExpandEnvironmentStringsW(target, expanded, ARRAYSIZE(expanded));
        exe = ToLower(expanded);
    }
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(sl.As(&ps))) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) aumid = pv.pwszVal;
        PropVariantClear(&pv);
    }
    return true;
}

// The taskbar's own pin list (binary PIDL blob). Store (MSIX) apps are pinned only here - as
// "PackageFamily!AppId" - while desktop apps also have a .lnk; the blob gives the pin order.
// Its items put their UTF-16 strings at odd byte offsets as often as at even ones (25H2, 2026-10-28: "Google
// Chrome.lnk" at byte 223, a store app's "Family!App" at 2035): the blob is read as text from byte 0 and from
// byte 1, and places in it are counted in bytes.
struct TaskbandText { std::wstring at[2]; };   // at[k]: the blob from byte k
static TaskbandText TaskbandBlob() {
    TaskbandText t;
    DWORD size = 0;
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband";
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"Favorites", RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS || size < 2) return t;
    std::vector<BYTE> b(size);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"Favorites", RRF_RT_REG_BINARY, nullptr, b.data(), &size) != ERROR_SUCCESS) return t;
    b.resize(size);
    for (size_t k = 0; k < 2 && b.size() >= k + 2; ++k) {
        t.at[k].resize((b.size() - k) / 2);
        memcpy(t.at[k].data(), b.data() + k, t.at[k].size() * sizeof(wchar_t));
    }
    return t;
}

// Where a string is in the blob (its first byte), SIZE_MAX: not there.
static size_t FindInBlob(const TaskbandText& t, const std::wstring& s) {
    size_t best = SIZE_MAX;
    for (size_t k = 0; k < 2; ++k)
        if (const size_t p = t.at[k].find(s); p != std::wstring::npos) best = std::min(best, p * 2 + k);
    return best;
}

// AUMIDs of pinned store apps, in pin order, with their position in the blob
static std::vector<std::pair<size_t, std::wstring>> PackagedPins(const std::wstring& blob) {
    std::vector<std::pair<size_t, std::wstring>> out;
    auto isName = [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_'; };
    for (size_t bang = blob.find(L'!'); bang != std::wstring::npos; bang = blob.find(L'!', bang + 1)) {
        size_t b = bang; while (b > 0 && isName(blob[b - 1])) --b;
        size_t e = bang + 1; while (e < blob.size() && isName(blob[e])) ++e;
        std::wstring fam = blob.substr(b, bang - b), app = blob.substr(bang + 1, e - bang - 1);
        size_t us = fam.rfind(L'_');   // PackageName_publisherId (13 chars)
        if (us == std::wstring::npos || fam.size() - us - 1 != 13 || us < 2 || app.empty()) continue;
        std::wstring id = fam + L"!" + app;
        if (std::none_of(out.begin(), out.end(), [&](auto& p) { return p.second == id; })) out.push_back({ b, id });
    }
    return out;
}
// ...in both of the blob's readings, each once at its first place, in pin order (places in bytes)
static std::vector<std::pair<size_t, std::wstring>> PackagedPins(const TaskbandText& t) {
    std::vector<std::pair<size_t, std::wstring>> out;
    for (size_t k = 0; k < 2; ++k)
        for (auto& [pos, id] : PackagedPins(t.at[k])) {
            auto it = std::find_if(out.begin(), out.end(), [&](auto& p) { return p.second == id; });
            if (it == out.end()) out.push_back({ pos * 2 + k, id });
            else it->first = std::min(it->first, pos * 2 + k);
        }
    std::stable_sort(out.begin(), out.end(), [](auto& x, auto& y) { return x.first < y.first; });
    return out;
}

std::wstring PackageFamily(const std::wstring& aumid) {
    size_t bang = aumid.find(L'!');
    return bang == std::wstring::npos ? std::wstring() : ToLower(aumid.substr(0, bang));
}

// The taskbar's pins in its order (the blob's): its shortcuts in the User Pinned folder (lnk; one the blob does not
// name goes last, the taskbar's order unknown) and its store apps (aumid; pinned only in the blob).
struct TaskbarPin { size_t at = SIZE_MAX; std::wstring lnk, aumid; };
static std::vector<TaskbarPin> TaskbarPins() {
    std::vector<TaskbarPin> pins;
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) return pins;
    const std::wstring dir = std::wstring(p) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\";
    CoTaskMemFree(p);
    const TaskbandText blob = TaskbandBlob();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.lnk").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do pins.push_back({ FindInBlob(blob, fd.cFileName), dir + fd.cFileName, L"" });
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto& [pos, id] : PackagedPins(blob)) pins.push_back({ pos, L"", id });
    std::stable_sort(pins.begin(), pins.end(), [](auto& x, auto& y) { return x.at < y.at; });
    return pins;
}

void AppModel::ImportTaskbarPins() {
    for (const TaskbarPin& pin : TaskbarPins()) {
        auto a = NewItem();
        a->pinned = true;
        if (!pin.lnk.empty()) {
            a->name = FileStem(pin.lnk);
            a->launch = pin.lnk;
            ReadShortcut(pin.lnk, a->exe, a->aumid);
        } else {
            a->aumid = pin.aumid;
            a->launch = L"shell:AppsFolder\\" + pin.aumid;
        }
        LoadIconAndName(*a, nullptr);
        items.push_back(a);
        Log(L"imported pin %s (%s | %s)", a->name.c_str(), a->exe.c_str(), a->aumid.c_str());
    }
}

// Pins saved by versions that did not know about store apps: add the missing ones once.
void AppModel::MigratePins() {
    if (g_settings.pinsVersion >= 2) return;
    const TaskbandText blob = TaskbandBlob();
    int added = 0;
    for (auto& [pos, id] : PackagedPins(blob)) {
        std::wstring fam = PackageFamily(id);
        bool have = std::any_of(items.begin(), items.end(), [&](auto& it) { return !it->aumid.empty() && PackageFamily(it->aumid) == fam; });
        if (have) continue;
        auto a = NewItem();
        a->pinned = true;
        a->aumid = id;
        a->launch = L"shell:AppsFolder\\" + id;
        LoadIconAndName(*a, nullptr);
        items.insert(items.begin() + PinnedCount(), a);
        added++;
        Log(L"migrated store pin %s (%s)", a->name.c_str(), id.c_str());
    }
    if (added) { structureVersion++; SavePins(); }
    g_settings.pinsVersion = 2;
    g_settings.Save();
}

int AppModel::PinFile(const std::wstring& path, int index) {
    auto a = NewItem();
    a->pinned = true;
    a->launch = path;
    const bool shortcut = EndsWithI(path, L".lnk");
    if (shortcut) { ReadShortcut(path, a->exe, a->aumid); a->name = FileStem(path); }
    else a->exe = ToLower(path);
    // the same app on the dock already - as Refresh groups windows: by its id, else by its program while one of the
    // two has no id (a shortcut to another id of the program, a browser profile's, is a pin of its own): pinned or
    // moved there
    for (size_t i = 0; i < items.size(); ++i) {
        AppItem& it = *items[i];
        const bool sameId = !a->aumid.empty() && _wcsicmp(it.aumid.c_str(), a->aumid.c_str()) == 0;
        const bool sameProgram = !a->exe.empty() && it.exe == a->exe && (it.aumid.empty() || a->aumid.empty());
        if (!sameId && !sameProgram) continue;
        if (!it.pinned && shortcut) it.launch = path;   // a running app pinned by its shortcut: the pin starts that (its arguments)
        MoveTo(it, (int)i < index ? index - 1 : index, true);   // the dock's index counts it when it is left of the drop point
        for (size_t j = 0; j < items.size(); ++j)
            if (items[j].get() == &it) return (int)j;
        return index;
    }
    LoadIconAndName(*a, nullptr);
    int pc = PinnedCount();
    index = std::clamp(index, 0, pc);
    items.insert(items.begin() + index, a);
    structureVersion++;
    SavePins();
    dirty = true;
    return index;
}

void AppModel::SetPinned(AppItem& a, bool pinned) {
    if (a.pinned == pinned) return;
    if (pinned) {
        if (a.launch.empty()) {
            std::wstring nm;
            if (!a.aumid.empty() && AppsFolderItem(a.aumid, &nm)) a.launch = L"shell:AppsFolder\\" + a.aumid;
            else a.launch = a.exe;
        }
        MoveTo(a, PinnedCount(), true);
    } else {
        a.pinned = false;
        // move to the running section (or drop entirely when not running)
        auto it = std::find_if(items.begin(), items.end(), [&](auto& p) { return p.get() == &a; });
        if (it == items.end()) return;
        auto sp = *it;
        items.erase(it);
        if (sp->Running()) items.insert(items.begin() + PinnedCount(), sp);
        else ForgetPresses(sp->id);
        structureVersion++;
        SavePins();
    }
}

void AppModel::MoveTo(AppItem& a, int index, bool pin) {
    auto it = std::find_if(items.begin(), items.end(), [&](auto& p) { return p.get() == &a; });
    if (it == items.end()) return;
    auto sp = *it;
    items.erase(it);
    if (pin) {
        if (sp->launch.empty()) {
            std::wstring nm;
            if (!sp->aumid.empty() && AppsFolderItem(sp->aumid, &nm)) sp->launch = L"shell:AppsFolder\\" + sp->aumid;
            else sp->launch = sp->exe;
        }
        sp->pinned = true;
        index = std::clamp(index, 0, PinnedCount());
    } else {
        int pc = PinnedCount();
        index = std::clamp(index, pc, (int)items.size());
    }
    items.insert(items.begin() + index, sp);
    structureVersion++;
    if (sp->pinned) SavePins();   // an app not pinned moved among the running ones: the pins are as they were (nothing written)
}
