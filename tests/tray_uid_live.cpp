// How Windows 11 keeps its tray icon settings (HKCU\Control Panel\NotifyIconSettings) for a program whose icon id
// (uID) changes from run to run, as MSI Afterburner's does (its uID looks like a window handle). The test adds one
// icon of its own with one id, removes it, and adds it again with another id from another window, as a new run of
// such a program would; meanwhile it prints its own program's entries in NotifyIconSettings whenever they change.
// It only reads the registry; the icon is its own; no other program's window or icon is touched.
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kKey[] = L"Control Panel\\NotifyIconSettings";
constexpr UINT kCallback = WM_APP + 7;

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring FileName(const std::wstring& p) {
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

struct Entry {
    std::wstring name, path;
    bool hasUid = false; DWORD uid = 0;
    DWORD promoted = 0; int order = -1;
};

std::wstring g_exe;   // this program's file name, lower case

std::vector<Entry> Read() {
    std::vector<Entry> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
    std::vector<unsigned long long> order;
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(root, L"UIOrderList", nullptr, &type, nullptr, &cb) == ERROR_SUCCESS && type == REG_BINARY && cb >= 8) {
        order.resize(cb / 8);
        if (RegQueryValueExW(root, L"UIOrderList", nullptr, &type, (BYTE*)order.data(), &cb) != ERROR_SUCCESS) order.clear();
        else order.resize(cb / 8);
    }
    wchar_t name[64];
    for (DWORD i = 0;; ++i) {
        DWORD cch = ARRAYSIZE(name);
        LONG rc = RegEnumKeyExW(root, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        wchar_t buf[1024];
        DWORD bytes = sizeof(buf);
        if (RegGetValueW(k, nullptr, L"ExecutablePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &bytes) == ERROR_SUCCESS &&
            Lower(FileName(buf)) == g_exe) {
            Entry e;
            e.name = name;
            e.path = buf;
            DWORD v = 0, sz = sizeof(v);
            if (RegGetValueW(k, nullptr, L"UID", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) { e.hasUid = true; e.uid = v; }
            v = 0; sz = sizeof(v);
            if (RegGetValueW(k, nullptr, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) e.promoted = v;
            unsigned long long number = _wcstoui64(name, nullptr, 10);
            for (size_t j = 0; j < order.size(); ++j) if (order[j] == number) { e.order = (int)j; break; }
            out.push_back(e);
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
    return out;
}

std::wstring Describe(const std::vector<Entry>& es) {
    std::wstring s;
    wchar_t line[256];
    for (const auto& e : es) {
        swprintf_s(line, L"\n    entry %s: uid=%s%lu promoted=%lu order=%d", e.name.c_str(), e.hasUid ? L"" : L"none/", e.uid, e.promoted, e.order);
        s += line;
    }
    return s.empty() ? L" (none)" : s;
}

void Pump(DWORD ms) {
    ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        ULONGLONG now = GetTickCount64();
        if (now >= end) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)(end - now), QS_ALLINPUT);
    }
}

// Watches this program's entries for ms milliseconds; prints them when they change. Returns the last state.
std::vector<Entry> Watch(const wchar_t* stage, DWORD ms, std::wstring& last) {
    std::vector<Entry> es;
    ULONGLONG start = GetTickCount64(), end = start + ms;
    bool first = true;
    while (GetTickCount64() < end) {
        es = Read();
        std::wstring now = Describe(es);
        if (first || now != last) {
            wprintf(L"%s +%.1f s:%s\n", stage, (GetTickCount64() - start) / 1000.0, now.c_str());
            fflush(stdout);
            last = now;
            first = false;
        }
        Pump(250);
    }
    return es;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

HWND MakeWindow() {
    return CreateWindowExW(0, L"LiquidDockTrayUidLive", L"LiquidDock tray uid test", 0, 0, 0, 0, 0, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

bool Add(HWND h, UINT uid) {
    NOTIFYICONDATAW d{};
    d.cbSize = sizeof(d);
    d.hWnd = h;
    d.uID = uid;
    d.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    d.uCallbackMessage = kCallback;
    d.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    wcscpy_s(d.szTip, L"LiquidDock tray uid test");
    return Shell_NotifyIconW(NIM_ADD, &d) != FALSE;
}

bool Remove(HWND h, UINT uid) {
    NOTIFYICONDATAW d{};
    d.cbSize = sizeof(d);
    d.hWnd = h;
    d.uID = uid;
    return Shell_NotifyIconW(NIM_DELETE, &d) != FALSE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_exe = Lower(FileName(self));
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LiquidDockTrayUidLive";
    RegisterClassW(&wc);

    // hold <seconds> [icons]: icons (default 1) with new ids, up for the given seconds - to see which of them
    // Explorer shows on the taskbar while the program's entry is promoted
    if (argc >= 3 && wcscmp(argv[1], L"hold") == 0) {
        HWND h = MakeWindow();
        UINT uid = 0x10000u + (GetTickCount() & 0xfff0u);
        int icons = argc >= 4 ? _wtoi(argv[3]) : 1;
        std::wstring last;
        for (int i = 0; i < icons; ++i) wprintf(L"HOLD uid=%u: %s\n", uid + i, Add(h, uid + i) ? L"ok" : L"refused");
        Watch(L"HOLD", (DWORD)_wtoi(argv[2]) * 1000, last);
        for (int i = 0; i < icons; ++i) Remove(h, uid + i);
        DestroyWindow(h);
        return 0;
    }

    // a different id on every run of the test, like a program that uses its window handle as the id
    UINT first = 0x10000u + (GetTickCount() & 0xfff0u), second = first + 0x8000u;
    std::wstring last;
    Watch(L"BEFORE", 500, last);

    HWND a = MakeWindow();
    wprintf(L"ADD uid=%u from window %p: %s\n", first, (void*)a, Add(a, first) ? L"ok" : L"refused");
    std::vector<Entry> afterFirst = Watch(L"FIRST", 8000, last);
    wprintf(L"DELETE uid=%u: %s\n", first, Remove(a, first) ? L"ok" : L"refused");
    DestroyWindow(a);
    Watch(L"GONE", 2000, last);

    HWND b = MakeWindow();   // a new run: another window, another id
    wprintf(L"ADD uid=%u from window %p: %s\n", second, (void*)b, Add(b, second) ? L"ok" : L"refused");
    std::vector<Entry> afterSecond = Watch(L"SECOND", 8000, last);
    wprintf(L"DELETE uid=%u: %s\n", second, Remove(b, second) ? L"ok" : L"refused");
    DestroyWindow(b);
    Watch(L"END", 2000, last);

    auto has = [](const std::vector<Entry>& es, UINT uid) {
        for (const auto& e : es) if (e.hasUid && e.uid == uid) return true;
        return false;
    };
    const wchar_t* verdict =
        afterSecond.empty() ? L"no entry seen while the icons were up (Windows writes them later)"
        : has(afterSecond, second) && has(afterSecond, first) ? L"one entry per id: a new id makes a new entry (the old one stays)"
        : has(afterSecond, second) ? L"one entry per program: the new id replaced the old one in it"
        : has(afterSecond, first) ? L"one entry per program, keeping the first id: the new id is matched by the program's path"
        : afterSecond.size() == 1 && !has(afterSecond, second)
            ? L"one entry per program, keeping the id of an earlier run: new ids are matched by the program's path"
        : L"unclear";
    wprintf(L"VERDICT: %s (entries after the first add %zu, after the second %zu)\n", verdict, afterFirst.size(), afterSecond.size());

    // two icons at once, both with ids Windows has not seen: does the second get an entry of its own?
    HWND c = MakeWindow();
    UINT third = second + 0x8000u, fourth = second + 0x8001u;
    wprintf(L"ADD uid=%u and uid=%u from window %p: %s %s\n", third, fourth, (void*)c, Add(c, third) ? L"ok" : L"refused",
            Add(c, fourth) ? L"ok" : L"refused");
    std::vector<Entry> afterTwo = Watch(L"TWO", 8000, last);
    Remove(c, third);
    Remove(c, fourth);
    DestroyWindow(c);
    Watch(L"END2", 2000, last);
    wprintf(L"VERDICT2: %zu entries with two icons up (%s)\n", afterTwo.size(),
            has(afterTwo, third) || has(afterTwo, fourth) ? L"a new id has an entry of its own" : L"no entry carries the new ids");
    return 0;
}
