#include "../src/common.h"
#include <fstream>
#include <iterator>
#include <cstdio>

static int checks = 0, failures = 0;
static void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; printf("FAIL: %s\n", name); }
}
int wmain() {
    const auto dir = AppDataDir();
    // This executable is built in its own fixture directory. Never touch installed data.
    if (dir.find(L"\\tests\\settings-options\\data") == std::wstring::npos) return 2;
    const auto path = dir + L"\\settings.ini";
    const bool existed = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::ifstream originalFile(path, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(originalFile)), {});
    if (existed && !originalFile) return 2;
    originalFile.close();
    struct Restore {
        std::wstring path; bool existed; std::string original;
        ~Restore() {
            if (existed) { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << original; }
            else DeleteFileW(path.c_str());
        }
    } restore{path, existed, original};
    auto write = [&](const std::string& text) { std::ofstream f(path, std::ios::trunc); f << text; };
    const std::string legacy = "edge=1\niconSize=54\nmagnification=2.1\nglassStyle=2\nshowDelay=0.15\nhideDelay=0.7\nautostart=1\ncpuTemp=0\npinsVersion=2\n";
    write(legacy);
    Settings s; s.Load();
    Check(s.trayButtons == "desktop,theme" && s.clockSize == 0 && s.iconSpacing == 1, "legacy settings get new defaults");
    write(legacy + "showDesktopButton=0\nshowThemeButton=1\nshowOledButton=1\n");
    Settings migrated; migrated.Load();
    Check(migrated.trayButtons == "theme,oled", "the old button switches become the button list");
    write(legacy + "trayButtons=\nshowDesktopButton=1\n");
    Settings emptied; emptied.Load();
    Check(emptied.trayButtons.empty(), "an empty button list stays empty (old switches ignored)");
    emptied.Save();
    { std::ifstream in(path); std::string all((std::istreambuf_iterator<char>(in)), {}); Check(all.find("showDesktopButton") == std::string::npos && all.find("trayButtons=") != std::string::npos, "old switches are no longer written"); }
    auto legacyUnchanged = [](const Settings& v) {
        return v.edge == Edge::Left && v.iconSize == 54 && v.magnification == 2.1f && v.glassStyle == 2 &&
               v.showDelay == 0.15f && v.hideDelay == 0.7f && v.autostart && v.sensorSource == 1 && v.pinsVersion == 2;
    };
    Check(legacyUnchanged(s), "legacy preferences survive load");
    for (bool desktop : {false, true}) for (int clock = 0; clock < 2; ++clock) for (int spacing = 0; spacing < 3; ++spacing) {
        s.trayButtons = desktop ? "desktop,theme" : "theme"; s.clockSize = clock; s.iconSpacing = spacing; s.Save();
        Settings loaded; loaded.Load();
        Check(loaded.trayButtons == (desktop ? "desktop,theme" : "theme") && loaded.clockSize == clock && loaded.iconSpacing == spacing,
              "all new option combinations survive save and fresh load");
        Check(legacyUnchanged(loaded), "saving new options preserves legacy preferences");
    }
    write(legacy + "showDesktopButton=invalid\nclockSize=invalid\niconSpacing=invalid\n");
    Settings invalid; invalid.Load();
    Check(invalid.trayButtons == "desktop,theme" && invalid.clockSize == 0 && invalid.iconSpacing == 1, "malformed new values use defaults");
    Check(legacyUnchanged(invalid), "malformed new values do not discard legacy preferences");
    write(legacy + "clockSize=99\niconSpacing=-5\n");
    Settings bounded; bounded.Load();
    Check(bounded.clockSize == 1 && bounded.iconSpacing == 0, "numeric option values are bounded");
    write(legacy + "dockThickness=999\nedgeGap=-3\nedgeOffset=-250\n");
    Settings shaped; shaped.Load();
    Check(shaped.dockThickness == 150 && shaped.edgeGap == 0 && shaped.edgeOffset == -100, "shape values are bounded");
    Settings fresh; fresh.Load();
    shaped.dockThickness = 120; shaped.edgeGap = 18; shaped.edgeOffset = 35; shaped.Save();
    Settings reread; reread.Load();
    Check(reread.dockThickness == 120 && reread.edgeGap == 18 && reread.edgeOffset == 35 && legacyUnchanged(reread), "shape values survive save and load");
    write(legacy);
    Settings legacyShape; legacyShape.Load();
    Check(legacyShape.dockThickness == 100 && legacyShape.edgeGap == 6 && legacyShape.edgeOffset == 0, "older files get the default shape");
    write(legacy);
    Settings hidden; hidden.Load();
    Check(hidden.autoHide, "older files keep auto-hide on");
    hidden.autoHide = false; hidden.Save();
    Settings shown; shown.Load();
    Check(!shown.autoHide && legacyUnchanged(shown), "auto-hide off survives save and load");
    write(legacy + "iconGap=99\n");
    Settings gapped; gapped.Load();
    Check(gapped.iconGap == 60 && gapped.iconSpacing == 1, "icon gap is bounded and leaves the preset alone");
    write(legacy + "magnification=2.5\n");
    Settings magnified; magnified.Load();
    Check(magnified.magnification == 2.1f, "magnification above the menu maximum is clamped");
    write(legacy + "clockSize=1\n");
    Settings partial; partial.Load();
    Check(partial.clockSize == 1 && partial.trayButtons == "desktop,theme" && partial.iconSpacing == 1, "partially upgraded files retain defaults for missing keys");
    // the sensor settings: the source and the six items; the old PawnIO switch carried over once
    {
        const Settings d;
        Check(d.sensorSource == 1 && d.showCpuTemp && d.showGpu && d.showVram && d.showVramTemp && d.showRamTemp && d.showSsd && !d.cpuTemp,
              "a fresh install: the source HWiNFO Gadget, every item shown");
        write("edge=0\ncpuTemp=0\n");
        Settings off; off.Load();
        Check(off.sensorSource == 1, "cpuTemp=0 (an older file): the source HWiNFO Gadget");
        write("edge=0\ncpuTemp=1\n");
        Settings on; on.Load();
        Check(on.sensorSource == 2 && on.sourceFromCpuTemp, "cpuTemp=1 (an older file): the source PawnIO, to be settled at the sensors' start");
        Check(!off.sourceFromCpuTemp, "cpuTemp=0: nothing to settle");
        write("edge=0\n");
        Settings before; before.Load();
        Check(before.sensorSource == 1, "a file from before both: HWiNFO Gadget");
        write("autostart=1\n");
        Settings installer; installer.Load();
        Check(installer.sensorSource == 1, "the installer's first file: HWiNFO Gadget");
        write("edge=0\ncpuTemp=1\nsensorSource=0\n");
        Settings explicitOff; explicitOff.Load();
        write("edge=0\nsensorSource=1\ncpuTemp=1\n");
        Settings explicitHw; explicitHw.Load();
        Check(explicitOff.sensorSource == 0 && explicitHw.sensorSource == 1, "a sensorSource line wins over the old cpuTemp, before or after it");
        Check(!explicitOff.sourceFromCpuTemp && !explicitHw.sourceFromCpuTemp, "...and nothing to settle then");
        write("sensorSource=7\n");
        Settings high; high.Load();
        write("sensorSource=-2\n");
        Settings low; low.Load();
        Check(high.sensorSource == 2 && low.sensorSource == 0, "sensorSource is bounded to 0..2");
        on.Save();
        { std::ifstream in(path); std::string all((std::istreambuf_iterator<char>(in)), {});
          Check(all.find("sensorSource=2") != std::string::npos && all.find("cpuTemp") == std::string::npos,
                "saved: sensorSource=2 written, cpuTemp no longer"); }
        Settings again; again.Load();
        Check(again.sensorSource == 2, "...and loaded again: PawnIO (no cpuTemp line, the sensorSource one)");
        Settings items; items.showCpuTemp = false; items.showGpu = false; items.showVram = true; items.showVramTemp = false;
        items.showRamTemp = false; items.showSsd = false; items.sensorSource = 0; items.Save();
        Settings back; back.Load();
        Check(back.sensorSource == 0 && !back.showCpuTemp && !back.showGpu && back.showVram && !back.showVramTemp && !back.showRamTemp && !back.showSsd,
              "the source and the six items survive save and load");
        write(legacy);
        Settings old; old.Load();
        Check(old.showCpuTemp && old.showGpu && old.showVram && old.showVramTemp && old.showRamTemp && old.showSsd, "older files show every item");
        write("showGpu=0\nshowSsd=0\n");
        Settings some; some.Load();
        Check(!some.showGpu && !some.showSsd && some.showCpuTemp && some.showVram && some.sensorSource == 1, "only the keys in the file change");
    }
    printf("RESULT settings checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
