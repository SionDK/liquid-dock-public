# Task T2: edge-case tests for src/fluent_settings.h

Write `tests/llm/fluent_settings_more_test.cpp`, a test program for `src/fluent_settings.h` (below): the dock's reader for FluentFlyout's settings file, which .NET's XmlSerializer writes.

Requirements:
- Include exactly `#include "src/fluent_settings.h"` and `#include "tests/portable_check.h"` (the build adds the repository root to the include path), plus any standard headers you need.
- `int main()` calls `Check(condition, "what this check pins down")` for every check and ends with `return Summary("fluent_settings_more_test");`.
- Use wide strings (`L"..."`, `std::wstring`) as the functions do.
- At least 25 checks, each a distinct case. Group them by function with a short comment.
- Cover every function: `Trimmed`, `XmlText`, `Element`, `ReadBool`, `ReadInt`, `ReadList`, `Parse`.
- Add cases the existing test (below) does not have, for example: tabs, `\r` and `\n` around values; every one of the five entities on its own and next to each other; `&amp;` followed by an entity name (decoded once only); an element name that is a prefix of another element name; an element that appears twice (the first one counts); `ReadBool` with `TRUE`, `1`, `yes`, an empty value and a missing element (fallback); `ReadInt` with `+5`, `-0`, ` 7 `, `12abc` (std::stoi reads the leading number), a value that is not a number, a value too large for int, a missing element (fallback); `ReadList` with entities in entries, a self-closing `<string />`, a missing list; `Parse` of a text without `<UserSettings`, of an empty `<UserSettings>` (FluentFlyout's defaults, including `animationSpeed` 2), of a file where only some values are present, and of `AppFilteringMode` 1.
- Expectations must follow from the code: for example `ReadBool` accepts exactly `true` after trimming, and `ReadInt` falls back only when `std::stoi` throws.

Your test is also run against copies of the header with one deliberate bug each (for example an entity decoded in the wrong order, or a default changed). A good test fails on most of them, so check exact values.
