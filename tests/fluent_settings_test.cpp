// FluentFlyout's settings file as the dock reads it (src/fluent_settings.h). No Windows headers: builds
// with any C++20 compiler.
//   cl /std:c++20 /EHsc /utf-8 tests\fluent_settings_test.cpp
//   g++ -std=c++20 tests/fluent_settings_test.cpp -o fluent_settings_test
#include "../src/fluent_settings.h"
#include "portable_check.h"

using namespace fluentsettings;

int main() {
    // the shape FluentFlyout 2.15 writes (from a real file, shortened)
    const std::wstring real = LR"(<?xml version="1.0" encoding="utf-8"?>
<UserSettings xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xsd="http://www.w3.org/2001/XMLSchema">
  <CompactLayout>true</CompactLayout>
  <FlyoutSelectedMonitor>1</FlyoutSelectedMonitor>
  <Position>0</Position>
  <FlyoutAnimationSpeed>2</FlyoutAnimationSpeed>
  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>
  <TaskbarWidgetSelectedMonitor>1</TaskbarWidgetSelectedMonitor>
  <TaskbarWidgetAutoHide>false</TaskbarWidgetAutoHide>
  <TaskbarWidgetPosition>0</TaskbarWidgetPosition>
  <TaskbarWidgetManualPadding>0</TaskbarWidgetManualPadding>
  <TaskbarWidgetHideCompletely>false</TaskbarWidgetHideCompletely>
  <TaskbarVisualizerEnabled>true</TaskbarVisualizerEnabled>
  <AppFilteringEnabled>false</AppFilteringEnabled>
  <AppFilteringMode>0</AppFilteringMode>
  <AllowedApps />
  <BlockedApps />
</UserSettings>
)";
    Settings s = Parse(real);
    Check(s.loaded, "a real settings file is recognised");
    Check(s.widgetEnabled && !s.hideCompletely && s.visualizer, "switches");
    Check(s.widgetMonitor == 1, "TaskbarWidgetSelectedMonitor, not FlyoutSelectedMonitor (both are 1 here)");
    Check(s.animationSpeed == 2 && s.manualPadding == 0 && !s.filtering && s.filterMode == 0, "numbers");
    Check(s.allowed.empty() && s.blocked.empty(), "empty lists written as <AllowedApps />");

    const std::wstring other = L"<UserSettings>\n<FlyoutSelectedMonitor>3</FlyoutSelectedMonitor>\n</UserSettings>";
    Check(Parse(other).widgetMonitor == 0, "the flyout's monitor is not the widget's monitor");

    Settings d = Parse(L"<UserSettings>\r\n</UserSettings>");
    Check(d.loaded && !d.widgetEnabled && !d.hideCompletely && !d.visualizer && d.widgetMonitor == 0 && d.animationSpeed == 2,
          "missing values: FluentFlyout's defaults");
    Check(!Parse(L"<html><TaskbarWidgetEnabled>true</TaskbarWidgetEnabled></html>").loaded, "another file is not taken for the settings");
    Check(!Parse(L"").loaded, "an empty file is not taken");

    const std::wstring tricky = LR"(<UserSettings>
  <TaskbarWidgetSelectedMonitorX>5</TaskbarWidgetSelectedMonitorX>
  <TaskbarWidgetSelectedMonitor> 2 </TaskbarWidgetSelectedMonitor>
  <TaskbarWidgetSelectedMonitor>3</TaskbarWidgetSelectedMonitor>
  <TaskbarWidgetManualPadding>-12</TaskbarWidgetManualPadding>
  <FlyoutAnimationSpeed>fast</FlyoutAnimationSpeed>
  <AppFilteringMode>99999999999</AppFilteringMode>
  <TaskbarWidgetEnabled>True</TaskbarWidgetEnabled>
  <TaskbarWidgetHideCompletely>
    true
  </TaskbarWidgetHideCompletely>
  <AppFilteringEnabled>true</AppFilteringEnabled>
  <BlockedApps>
    <string>Spotify</string>
    <string>Tom &amp; Jerry &lt;3</string>
    <string>&amp;lt;not an entity&amp;gt;</string>
    <string />
  </BlockedApps>
</UserSettings>)";
    Settings t = Parse(tricky);
    Check(t.widgetMonitor == 2, "a longer element name sharing the prefix is not taken; spaces around the number are");
    Check(t.manualPadding == -12, "negative numbers");
    Check(t.animationSpeed == 2, "not a number: the default");
    Check(t.filterMode == 0, "a number too big for int: the default");
    Check(!t.widgetEnabled, "XmlSerializer writes lowercase true; anything else is false");
    Check(t.hideCompletely, "a value on its own line still counts");
    Check(t.filtering && t.blocked.size() == 3, "list entries; an empty <string /> is no entry");
    Check(t.blocked.size() == 3 && t.blocked[1] == L"Tom & Jerry <3" && t.blocked[2] == L"&lt;not an entity&gt;",
          "entities decoded once (\"&amp;lt;\" stays the text \"&lt;\")");

    Check(XmlText(L"a &quot;b&quot; &apos;c&apos; &gt; d") == L"a \"b\" 'c' > d", "all five entities");
    Check(XmlText(L"&amp;amp;") == L"&amp;", "&amp;amp; is the text &amp;");
    Check(Trimmed(L" \t x y \r\n") == L"x y" && Trimmed(L"   ").empty() && Trimmed(L"").empty(), "trimming");
    std::wstring out;
    Check(!Element(L"<A>1", L"A", out), "an unclosed element counts as missing");
    Check(Element(L"<A></A>", L"A", out) && out.empty(), "an empty element gives empty text");

    // more of what XmlSerializer writes: non-ASCII names, a null list, markup inside a name, a longer
    // element name sharing a list's prefix, tabs and CRLF, a negative monitor setting
    const std::wstring lists = L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        L"<UserSettings xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
        L"\t<AllowedAppsBackup>\r\n\t\t<string>old</string>\r\n\t</AllowedAppsBackup>\r\n"
        L"\t<TaskbarWidgetSelectedMonitor>-1</TaskbarWidgetSelectedMonitor>\r\n"
        L"\t<TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n"
        L"\t<AppFilteringMode>1</AppFilteringMode>\r\n"
        L"\t<AllowedApps>\r\n\t\t<string>멜론</string>\r\n\t\t<string>Spotify</string>\r\n\t</AllowedApps>\r\n"
        L"\t<BlockedApps>\r\n\t\t<string>&lt;/BlockedApps&gt;</string>\r\n\t\t<string>벅스 뮤직</string>\r\n\t</BlockedApps>\r\n"
        L"</UserSettings>\r\n";
    Settings l = Parse(lists);
    Check(l.loaded && l.widgetEnabled && l.widgetMonitor == -1 && l.filterMode == 1, "tabs and CRLF; a negative monitor setting is read as is (FluentFlyout clamps it)");
    Check(l.allowed.size() == 2 && l.allowed[0] == L"멜론" && l.allowed[1] == L"Spotify",
          "non-ASCII names; a longer element name sharing the list's prefix is not the list");
    Check(l.blocked.size() == 2 && l.blocked[0] == L"</BlockedApps>" && l.blocked[1] == L"벅스 뮤직",
          "an escaped closing tag inside a name does not end the list");
    Settings nil = Parse(L"<UserSettings>\n<AllowedApps xsi:nil=\"true\" />\n<BlockedApps />\n</UserSettings>");
    Check(nil.loaded && nil.allowed.empty() && nil.blocked.empty(), "a null list (xsi:nil) is an empty list");
    return Summary("fluent_settings_test");
}
