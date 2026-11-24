// The tray host's part files (src\tray_host*.cpp), in the order the tests include them under their fakes:
// tray_host_test, tray_host_edge_test, fluent_flyout_test. Each test defines the LD_TRAY_* names and its
// ExplorerTaskbar fake before this include and #undefs ExplorerTaskbar after it (fluent_flyout_test after
// FluentFlyout's parts). A new part of the tray host goes here and into tests\lists.bat's DOCK_MODULES and
// PROBE_MODULES (the dock tests link it).
#include "../src/tray_host.cpp"
#include "../src/tray_host_icons.cpp"
#include "../src/tray_host_settings.cpp"
#include "../src/tray_host_widget.cpp"
