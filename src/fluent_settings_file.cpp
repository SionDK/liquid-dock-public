#include "fluent_internal.h"

namespace fluentflyout {

namespace {

// ---------------------------------------------------------------- FluentFlyout's settings
// %APPDATA%\FluentFlyout\settings.xml (fluent_settings.h). The Microsoft Store version keeps it in its
// package's private copy of AppData. Read again whenever the file changes.
FfSettings ParseSettings(const std::wstring& xml) { return fluentsettings::Parse(xml); }

bool ReadUtf8File(const std::wstring& path, std::wstring& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (8 << 20);
    std::string bytes;
    if (ok) {
        bytes.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr) && got == bytes.size();
    }
    CloseHandle(f);
    if (!ok) return false;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) bytes.erase(0, 3);
    out = FromUtf8(bytes);
    return true;
}

#ifdef LD_FLUENT_SETTINGS_FILE
std::vector<std::wstring> SettingsFiles() { return { LD_FLUENT_SETTINGS_FILE() }; }
#else
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) s = p;
    CoTaskMemFree(p);
    return s;
}

std::vector<std::wstring> SettingsFiles() {
    std::vector<std::wstring> out;
    std::wstring roaming = KnownFolder(FOLDERID_RoamingAppData), local = KnownFolder(FOLDERID_LocalAppData);
    if (!roaming.empty()) out.push_back(roaming + L"\\FluentFlyout\\settings.xml");
    if (!local.empty()) {
        std::wstring packages = local + L"\\Packages\\";
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((packages + L"unchihugo.FluentFlyout_*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    out.push_back(packages + fd.cFileName + L"\\LocalCache\\Roaming\\FluentFlyout\\settings.xml");
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    return out;
}
#endif

}   // namespace

FfSettings g_ff;
int g_ffVersion = 0;
std::wstring g_ffPath;
FILETIME g_ffTime{};
double g_nextSettingsCheck = 0;
double g_ffMissingSince = -1;               // FluentFlyout replaces the file when saving: a moment without it
std::vector<std::wstring> g_settingsFiles;  // where it may be (the Store version's folder name is looked up)
double g_settingsFilesAt = -1;
int g_fileSetting = -1;                     // its widget monitor setting (0-based), -1 until read
double g_fileAt = -1;                       // when that value was read as new
bool g_choiceDirty = true;                  // the setting or the monitors changed: work the monitor out again
int g_fileMonitor = -1;                     // g_fileSetting as a position among the monitors (UpdateMonitor)
std::atomic<bool> g_namesWanted{ false };   // found, its app filter on: the media sessions' apps are named (PublishSessions)

namespace {

// The places its settings file may be, looked up again every 30 s (and right away while none exists):
// enumerating the Packages folder on every check is not needed.
const std::vector<std::wstring>& KnownSettingsFiles(double now) {
    if (g_settingsFilesAt < 0 || now - g_settingsFilesAt > 30) {
        g_settingsFiles = SettingsFiles();
        g_settingsFilesAt = now;
    }
    return g_settingsFiles;
}

}   // namespace

void LoadSettings(double now, bool force) {
    if (!force && now < g_nextSettingsCheck) return;
    g_nextSettingsCheck = now + 2.0;
    std::wstring best;
    FILETIME bestTime{};
    for (auto& p : KnownSettingsFiles(now)) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) continue;
        if (best.empty() || CompareFileTime(&a.ftLastWriteTime, &bestTime) > 0) { best = p; bestTime = a.ftLastWriteTime; }
    }
    if (best.empty()) {
        g_settingsFilesAt = -1;   // look for the folder again next time
        if (g_ffMissingSince < 0) g_ffMissingSince = now;
        if (g_ff.loaded && now - g_ffMissingSince > 10) {
            g_ff = {}; g_ffPath.clear(); ++g_ffVersion;
            g_namesWanted = false;
            g_fileSetting = -1; g_fileAt = now; g_choiceDirty = true;
            Log(L"fluentflyout: settings file gone - its defaults assumed");
        }
        return;
    }
    g_ffMissingSince = -1;
    if (best == g_ffPath && CompareFileTime(&bestTime, &g_ffTime) == 0) return;
    std::wstring xml;
    if (!ReadUtf8File(best, xml)) return;   // being replaced right now: next time
    FfSettings s = ParseSettings(xml);
    if (!s.loaded) return;
    g_ffPath = best; g_ffTime = bestTime;
    if (s.widgetMonitor != g_fileSetting) { g_fileSetting = s.widgetMonitor; g_fileAt = now; g_choiceDirty = true; }
    g_ff = s;
    ++g_ffVersion;
    g_namesWanted = s.filtering;   // the media thread names the apps from its next list or check (within 5 s)
    Log(L"fluentflyout: settings %s - widget %d, hide when nothing plays %d, monitor %d, visualizer %d, app filter %d (%s), animation %d",
        best.c_str(), (int)s.widgetEnabled, (int)s.hideCompletely, s.widgetMonitor, (int)s.visualizer, (int)s.filtering,
        s.filterMode == 0 ? L"block" : L"allow", s.animationSpeed);
}

}   // namespace fluentflyout
