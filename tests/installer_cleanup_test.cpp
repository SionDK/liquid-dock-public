// Exercises the production retirement and cleanup functions on freshly created fixtures.
// Never calls Install(), Uninstall(), StopDock(), or the production installer entry point.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool failRetirement = false;
static BOOL WINAPI TestMoveFileEx(LPCWSTR from, LPCWSTR to, DWORD flags) {
    if (failRetirement) { SetLastError(ERROR_SHARING_VIOLATION); return FALSE; }
    return MoveFileExW(from,to,flags);
}
#define MoveFileExW TestMoveFileEx
#define wWinMain UnusedInstallerEntryPoint
#include "../installer/setup_main.cpp"
#undef wWinMain
#undef MoveFileExW
#include <winioctl.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <vector>
namespace fs = std::filesystem;

static void Check(bool ok, const char* label) {
    if (!ok) { std::printf("FAIL: %s (Windows error %lu)\n",label,GetLastError()); std::exit(1); }
}
static void Write(const fs::path& path, const char* contents) {
    std::ofstream file(path, std::ios::binary); file << contents; file.close(); Check(!file.fail(), "write fixture");
}
static std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
// A directory junction (as mklink /J makes, no privilege needed): link -> target.
static bool MakeJunction(const fs::path& link, const fs::path& target) {
    std::error_code ec;
    if (!fs::create_directory(link, ec)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    // REPARSE_DATA_BUFFER (ntifs.h) for a mount point: tag, data length, reserved, then the substitute and
    // print names' offsets and lengths, then both names, each ending in a zero
    std::wstring sub = L"\\??\\" + target.wstring(), print = target.wstring();
    std::vector<BYTE> buf(16 + (sub.size() + 1 + print.size() + 1) * sizeof(wchar_t));
    struct Head { DWORD tag; WORD dataLength, reserved, subOffset, subLength, printOffset, printLength; };
    Head* head = reinterpret_cast<Head*>(buf.data());
    head->tag = IO_REPARSE_TAG_MOUNT_POINT;
    head->dataLength = WORD(buf.size() - 8);
    head->subLength = WORD(sub.size() * sizeof(wchar_t));
    head->printOffset = WORD((sub.size() + 1) * sizeof(wchar_t));
    head->printLength = WORD(print.size() * sizeof(wchar_t));
    memcpy(buf.data() + 16, sub.c_str(), (sub.size() + 1) * sizeof(wchar_t));
    memcpy(buf.data() + 16 + head->printOffset, print.c_str(), (print.size() + 1) * sizeof(wchar_t));
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), DWORD(buf.size()), nullptr, 0, &bytes, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && !wcscmp(argv[1], L"--hold")) {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[2]);
        HANDLE quit = OpenEventW(SYNCHRONIZE,FALSE,argv[3]);
        if (!ready || !quit) return 2;
        SetEvent(ready); WaitForSingleObject(quit,30000); CloseHandle(ready); CloseHandle(quit);
        return 0;
    }
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM init");
    wchar_t exe[MAX_PATH]; Check(GetModuleFileNameW(nullptr,exe,ARRAYSIZE(exe)) != 0,"test executable path");
    fs::path testBin = fs::path(exe).parent_path();
    // build\tests, or a folder of its own under it (build\tests\<name>)
    Check((testBin.filename() == L"tests" && testBin.parent_path().filename() == L"build") ||
          (testBin.parent_path().filename() == L"tests" && testBin.parent_path().parent_path().filename() == L"build"),
          "fixture root must be build/tests");
    GUID id; wchar_t guid[40]; Check(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id,guid,ARRAYSIZE(guid)),"fixture id");
    fs::path root = testBin / (L"installer-fixture-" + std::wstring(guid));
    Check(!fs::exists(root),"fresh isolated fixture only");
    fs::create_directory(root);
    // Percent, exclamation, ampersand, parentheses, spaces and Unicode must survive cmd parsing.
    SetEnvironmentVariableW(L"LIQUIDDOCK_TEST_SENTINEL",L"EXPANDED");
    fs::path parent = root / L"한글 %LIQUIDDOCK_TEST_SENTINEL% & ! (test)";
    fs::path active = parent / L"LiquidDock";
    fs::create_directories(active / L"data");
    Write(active / L"data" / L"settings.ini","old installation");
    Check(CopyFileW(exe,(active / L"uninstall.exe").c_str(),TRUE) != FALSE,"copy running-image fixture");

    std::wstring eventBase = L"Local\\LiquidDock.CleanupTest." + std::wstring(guid);
    HANDLE ready = CreateEventW(nullptr,TRUE,FALSE,(eventBase + L".ready").c_str());
    HANDLE quit = CreateEventW(nullptr,TRUE,FALSE,(eventBase + L".quit").c_str());
    Check(ready && quit,"fixture events");
    std::wstring command = L"\"" + (active / L"uninstall.exe").wstring() + L"\" --hold " + eventBase + L".ready " + eventBase + L".quit";
    STARTUPINFOW si = {sizeof(si)}; si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION pi = {};
    Check(CreateProcessW((active/L"uninstall.exe").c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&si,&pi) != FALSE,"start old executable");
    CloseHandle(pi.hThread);
    Check(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0,"old executable running");

    std::wstring retired;
    Check(RetireInstallation(active.wstring(),retired),"retire directory while old executable is running");
    fs::path retiredPath = L"\\\\?\\" + retired;
    Check(fs::path(retired).parent_path()==active.parent_path() && !fs::exists(active),"retired directory stays in verified fixture parent");
    fs::create_directories(active/L"data");
    Write(active/L"data"/L"settings.ini","NEW SETTINGS MUST SURVIVE");
    Write(active/L"LiquidDock.exe","NEW BINARY MUST SURVIVE");
    Check(!ScheduleCleanup(active.wstring()),"active installation cannot be cleanup target");
    Check(!ScheduleCleanup(active.wstring()+L".removing-{invalid}"),"invalid retired identity rejected");
    Check(ScheduleCleanup(retired,1,4),"schedule old-generation cleanup");
    ULONGLONG start = GetTickCount64();
    while (fs::exists(retiredPath/L"data"/L"settings.ini") && GetTickCount64()-start<2500) Sleep(25);
    Check(!fs::exists(retiredPath/L"data"/L"settings.ini"),"first cleanup pass ran");
    Check(Read(active/L"data"/L"settings.ini")=="NEW SETTINGS MUST SURVIVE","new settings survive first pass");
    SetEvent(quit); Check(WaitForSingleObject(pi.hProcess,5000)==WAIT_OBJECT_0,"old executable exits");
    CloseHandle(pi.hProcess); CloseHandle(ready); CloseHandle(quit);
    // Let the delayed second pass complete before checking the new installation again.
    while (GetTickCount64()-start<5000) Sleep(25);
    Check(!fs::exists(retiredPath),"old generation removed after executable exits");
    Check(Read(active/L"data"/L"settings.ini")=="NEW SETTINGS MUST SURVIVE","new settings survive delayed second pass");
    Check(Read(active/L"LiquidDock.exe")=="NEW BINARY MUST SURVIVE","new binary survives delayed second pass");
    std::puts("PASS: immediate reinstall survives both cleanup passes, including special path characters");

    fs::path locked = root/L"LockedInstall";
    fs::create_directory(locked); Write(locked/L"keep.txt","preserve on failure");
    failRetirement = true;
    retired=L"unexpected";
    Check(!RetireInstallation(locked.wstring(),retired) && retired.empty() && Read(locked/L"keep.txt")=="preserve on failure","failed retirement never produces a cleanup target");
    failRetirement = false;
    std::puts("PASS: injected sharing violation retains the installation and produces no cleanup target");
    Check(!RetireInstallation((root/L"missing").wstring(),retired) && retired.empty(),"missing target refused");
    std::puts("PASS: missing directory and non-retired cleanup paths are rejected");

    // Junctions: the cleanup removes the old installation, never what a junction in it points to, and
    // an installation folder that is itself a junction is not retired (its target is not the dock's).
    fs::path outside = root/L"Outside";
    fs::create_directory(outside); Write(outside/L"keep.txt","outside the installation");
    fs::path linked = root/L"Linked"/L"LiquidDock";
    fs::create_directories(linked/L"data"); Write(linked/L"data"/L"settings.ini","old installation");
    Check(MakeJunction(linked/L"data"/L"elsewhere",outside) && fs::exists(linked/L"data"/L"elsewhere"/L"keep.txt"),"junction fixture");
    Check(RetireInstallation(linked.wstring(),retired),"retire installation that contains a junction");
    fs::path linkedRetired = L"\\\\?\\" + retired;
    Check(ScheduleCleanup(retired,1,2),"schedule its cleanup");
    start = GetTickCount64();
    while (fs::exists(linkedRetired) && GetTickCount64()-start<5000) Sleep(25);
    Check(!fs::exists(linkedRetired),"old installation with a junction removed");
    Check(Read(outside/L"keep.txt")=="outside the installation","what the junction points to survives the cleanup");
    std::puts("PASS: cleanup removes a junction inside the old installation without following it");
    fs::path junctionInstall = root/L"JunctionInstall"/L"LiquidDock";
    fs::create_directories(junctionInstall.parent_path());
    Check(MakeJunction(junctionInstall,outside),"junction installation fixture");
    retired = L"unexpected";
    Check(!RetireInstallation(junctionInstall.wstring(),retired) && retired.empty() && Read(outside/L"keep.txt")=="outside the installation",
          "an installation folder that is a junction is not retired");
    std::puts("PASS: a junction in place of the installation folder is refused");
    SetEnvironmentVariableW(L"LIQUIDDOCK_TEST_SENTINEL",nullptr);
    // Leave fixtures for inspection; only the production cleanup above deletes its exact retired directory.
    CoUninitialize();
    std::puts("Installer cleanup regression passed; actual user installation was not accessed.");
}
