#include "tray_host_internal.h"

namespace trayhost {

std::vector<Entry> g_entries;             // host thread only

namespace {

bool g_haveSettings = false;              // false before Windows 11 22H2: every icon counts as shown

bool ParseGuid(std::wstring s, GUID& g) {
    if (s.empty()) return false;
    if (s.front() != L'{') s = L"{" + s + L"}";
    return SUCCEEDED(IIDFromString(s.c_str(), &g));
}

std::wstring ResolveExecutablePath(const std::wstring& raw) {
    if (raw.size() > 38 && raw[0] == L'{' && raw[37] == L'}') {
        GUID folder{};
        if (SUCCEEDED(IIDFromString(raw.substr(0, 38).c_str(), &folder))) {
            PWSTR base = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &base)) && base) {
                std::wstring p = std::wstring(base) + raw.substr(38);
                CoTaskMemFree(base);
                return p;
            }
            if (base) CoTaskMemFree(base);
        }
    }
    return raw;
}

bool ReadString(HKEY k, const wchar_t* name, std::wstring& out) {
    wchar_t buf[1024];
    DWORD cb = sizeof(buf);
    if (RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &cb) != ERROR_SUCCESS) return false;
    out = buf;
    return true;
}

}   // namespace

void LoadEntries() {
    g_entries.clear();
    HKEY root = nullptr;
    g_haveSettings = RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &root) == ERROR_SUCCESS;
    if (!g_haveSettings) return;
    std::vector<uint64_t> order;
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(root, L"UIOrderList", nullptr, &type, nullptr, &cb) == ERROR_SUCCESS && type == REG_BINARY && cb >= 8) {
        order.resize(cb / 8);
        DWORD got = (DWORD)(order.size() * 8);
        if (RegQueryValueExW(root, L"UIOrderList", nullptr, &type, (BYTE*)order.data(), &got) == ERROR_SUCCESS) order.resize(got / 8);
        else order.clear();
    }
    wchar_t name[64];
    for (DWORD i = 0;; ++i) {
        DWORD cch = ARRAYSIZE(name);
        LONG rc = RegEnumKeyExW(root, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        Entry e;
        e.name = name;
        std::wstring s;
        if (ReadString(k, L"ExecutablePath", s)) e.exe = Lower(ResolveExecutablePath(s));
        if (ReadString(k, L"IconGuid", s)) e.hasGuid = ParseGuid(s, e.guid);
        DWORD v = 0, sz = sizeof(v);
        if (RegGetValueW(k, nullptr, L"UID", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) { e.hasUid = true; e.uid = v; }
        v = 0; sz = sizeof(v);
        e.promoted = RegGetValueW(k, nullptr, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v != 0;
        uint64_t number = _wcstoui64(name, nullptr, 10);
        for (size_t j = 0; j < order.size(); ++j) if (order[j] == number) { e.order = (int)j; break; }
        g_entries.push_back(std::move(e));
        RegCloseKey(k);
    }
    RegCloseKey(root);
}

namespace {

// Windows can keep several entries for one icon (stale ones from older runs): the one in its own
// order list is the live one, a promoted one next. An icon whose id has no entry follows the entry of its
// program when the program has just one: Windows does the same for a program whose id changes from run to
// run (MSI Afterburner's looks like a window handle) - it keeps one entry with the first id and shows every
// later id by it (tests\tray_uid_live.cpp, 25H2; the same log on 26H2). With more entries of the program
// there is no telling which one Windows applies: none, as before.
const Entry* FindEntry(const Icon& ic) {
    const Entry* best = nullptr;
    const Entry* program = nullptr;
    int programEntries = 0;
    auto rank = [](const Entry* e) { return (e->order != INT_MAX ? 2 : 0) + (e->promoted ? 1 : 0); };
    for (const auto& e : g_entries) {
        bool same;
        if (ic.hasGuid) same = e.hasGuid && IsEqualGUID(e.guid, ic.guid);
        else {
            if (e.hasGuid || ic.exe.empty() || e.exe != ic.exe) continue;
            ++programEntries;
            program = &e;
            same = e.hasUid && e.uid == ic.uid;
        }
        if (same && (!best || rank(&e) > rank(best))) best = &e;
    }
    if (!best && programEntries == 1) best = program;
    return best;
}

// The entry an icon got from its program (another id, see FindEntry), or nullptr.
const Entry* ProgramEntry(const Icon& ic) {
    if (ic.hasGuid || ic.entry.empty()) return nullptr;
    for (const auto& e : g_entries)
        if (e.name == ic.entry) return e.hasUid && e.uid == ic.uid ? nullptr : &e;
    return nullptr;
}

std::wstring FileName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// A dock.log line made now, written later (formatted as Log does: at most 2047 characters).
std::wstring Line(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

}   // namespace

// Lines made with g_mu held go to dock.log once it is let go: the dock's thread may be waiting for it
// (Shown, Image), and a line is a file write.
void LogLines(const std::vector<std::wstring>& lines) {
    for (const auto& l : lines) Log(L"%s", l.c_str());
}

// What the dock decided about an icon and why: one line for dock.log per new icon or change (into `out`).
void DescribeIcon(const Icon& ic, const wchar_t* what, const wchar_t* picture, std::vector<std::wstring>& out) {
    std::wstring name = ic.exe.empty() ? L"(unknown program)" : FileName(ic.exe);
    const Entry* byProgram = ProgramEntry(ic);
    std::wstring why = !g_haveSettings ? L"no NotifyIconSettings: all shown"
                     : byProgram ? L"entry " + ic.entry + L" of its program, made for uid " + std::to_wstring(byProgram->uid)
                     : !ic.entry.empty() ? L"entry " + ic.entry
                     : L"no settings entry yet";
    out.push_back(Line(L"tray host: %s %s uid=%u%s%s picture=%s %s (%s)", what, name.c_str(), ic.uid, ic.hasGuid ? L" guid" : L"",
                       ic.callback ? L"" : L" no-callback", picture, ic.hidden ? L"hidden by the program" : ic.promoted ? L"shown" : L"not shown",
                       why.c_str()));
    if (g_haveSettings && ic.entry.empty() && !ic.hasGuid && !ic.exe.empty()) {   // entries that nearly match: the cause
        int listed = 0;
        for (const auto& e : g_entries)
            if (!e.exe.empty() && FileName(e.exe) == name && listed++ < 3)
                out.push_back(Line(L"tray host:   candidate entry %s: %s uid=%s%u promoted=%d", e.name.c_str(), e.exe.c_str(),
                                   e.hasUid ? L"" : L"none/", e.uid, (int)e.promoted));
    }
}

// After the start-up re-registration: programs that Windows shows on the taskbar and that are running,
// but whose icon has not come (they ignore the request, or run as administrator and do not let it
// through). One line each, so a missing icon has a stated cause. arrived: the programs that sent one.
void LogMissing(const std::vector<std::wstring>& arrived) {
    if (!g_haveSettings) return;
    std::vector<std::wstring> wanted;
    for (const auto& e : g_entries)
        if (e.promoted && !e.exe.empty() && std::find(arrived.begin(), arrived.end(), e.exe) == arrived.end() &&
            std::find(wanted.begin(), wanted.end(), e.exe) == wanted.end())
            wanted.push_back(e.exe);
    if (wanted.empty()) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> reported;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        std::wstring file = Lower(pe.szExeFile);
        for (const auto& exe : wanted) {
            if (FileName(exe) != file || std::find(reported.begin(), reported.end(), exe) != reported.end()) continue;
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!p) continue;
            // an unread path is empty: never `exe`, which is not (wanted)
            bool same = Lower(ImagePath(p)) == exe;
            bool elevated = same && TokenElevated(p);
            CloseHandle(p);
            if (!same) continue;
            reported.push_back(exe);
            Log(L"tray host: %s is running and shown on the taskbar, but its icon has not come%s; if it is missing "
                L"on the dock, restart that program", FileName(exe).c_str(),
                elevated && !g_elevated ? L" (it runs as administrator, which can keep the request away)" : L"");
        }
    }
    CloseHandle(snap);
}

// g_mu held. True when anything changed.
bool UpdatePromotion(Icon& ic) {
    bool promoted = !g_haveSettings;
    int order = INT_MAX;
    if (g_haveSettings) {
        const Entry* e = FindEntry(ic);
        if (!e && !ic.hasGuid && !ic.hidden) {
            // Explorer may keep a program's entry under a hidden sibling icon of the same window
            // (Task Manager's spare icons) while the visible one has none: that entry counts.
            for (const auto& other : g_icons)
                if (&other != &ic && other.hwnd == ic.hwnd && other.hidden && !other.hasGuid)
                    if ((e = FindEntry(other)) != nullptr) break;
        }
        if (e) { promoted = e->promoted; order = e->order; }
        ic.entry = e ? e->name : std::wstring();
    }
    bool changed = promoted != ic.promoted || order != ic.order;
    ic.promoted = promoted; ic.order = order;
    return changed;
}

bool UpdateAllPromotion() {
    bool changed = false;
    std::vector<std::wstring> lines;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& ic : g_icons) {
            bool was = ic.promoted;
            if (!UpdatePromotion(ic)) continue;
            changed = true;
            if (was != ic.promoted) DescribeIcon(ic, L"now", ic.w > 0 ? L"ok" : L"none", lines);
        }
    }
    LogLines(lines);
    return changed;
}

}   // namespace trayhost
