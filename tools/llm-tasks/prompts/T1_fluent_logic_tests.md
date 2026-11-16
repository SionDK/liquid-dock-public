# Task T1: more tests for src/fluent_logic.h

Write `tests/llm/fluent_logic_more_test.cpp`, a test program for `src/fluent_logic.h` (below). That header decides which of the dock's monitors holds FluentFlyout's taskbar widget, when "nothing playing" counts, and how long the dock waits for the widget window to come back.

Requirements:
- Include exactly `#include "src/fluent_logic.h"` and `#include "tests/portable_check.h"` (the build adds the repository root to the include path), plus any standard headers you need.
- `int main()` calls `Check(condition, "what this check pins down")` for every check and ends with `return Summary("fluent_logic_more_test");`.
- At least 30 checks, each a distinct case. Group them by function with a short comment.
- Cover every function: `FluentOrder`, `MonitorForSetting`, `SettingNumber`, `PrimaryOf`, `WantedMonitor` (also the `Reason` it reports through `why`, and calling it without `why`), `Steady::Update`, `Steady::Waiting`, `StillWaiting`.
- Test exact boundaries. Use times that are exact in binary floating point (0.5, 1.0, 1.5, 3.0 ...): for example `now - offSince == hold` in `Steady::Update` and `now - lostAt == kRehomeSeconds` in `StillWaiting`.
- Add cases the existing test (below) does not have, for example: equal `left` values (the stable order keeps the system's order), several monitors marked primary, a list without a primary monitor, negative and very large settings, an empty monitor list, a monitor index that is not in the list, `shownAt == fileAt`, a file monitor of 0, a short "playing" in the middle of a "nothing playing" hold (it restarts the hold), `Waiting()` before, during and after a hold, `Update` returning true only when `value` changes, a custom `hold`, a negative `lostAt`.

Your test is also run against copies of the header with one deliberate bug each (for example a `<` turned into `<=`, or a fallback removed). A good test fails on most of them, so check exact values, not just "something came back".
