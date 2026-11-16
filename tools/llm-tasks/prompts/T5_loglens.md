# Task T5: tools/loglens.py, a log summary for bug reports

Write `tools/loglens.py`: a command-line tool that reads Liquid Dock's `dock.log` and FluentFlyout's `logs.<date>.txt` and prints counts that show at a glance what happened (how often FluentFlyout's widget moved or was re-created, whether media playback flickered, which problems were logged). Three sample files are below.

Python 3.8 or later, standard library only (`argparse`, `json`, `re`, `sys` are enough). The tool only reads the files it is given.

## Command line

`python tools/loglens.py FILE [FILE ...]`

- Prints ONE JSON object to standard output: `{"files": [entry, entry, ...]}`, one entry per FILE argument in the same order, written with `json.dumps(result, ensure_ascii=False, indent=2)` followed by a newline. Nothing else goes to standard output.
- Exit code 0 when every file could be read, 1 when at least one could not. Without any FILE: a usage message on standard error and exit code 2.

## Reading a file

- Read the bytes and decode them as `utf-8-sig` with `errors="replace"` (dock.log starts with a byte order mark). Split the text with `str.splitlines()`. Remove trailing whitespace from every line (`rstrip()`).
- A blank line is one that is empty after stripping. `"lines"` is the number of lines that are not blank.
- A file that cannot be read (missing, a folder, no permission) gives the entry `{"path": FILE, "error": "<a short message>"}`.

Every other entry is `{"path": FILE, "kind": KIND, "lines": N, ...}` where `path` is the argument exactly as given, and KIND comes from the first non-blank line:
- `"dock"` when it matches the regular expression `^\d{2}:\d{2}:\d{2}\.\d{3} `
- `"fluentflyout"` when it matches `^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+\|`
- `"unknown"` otherwise, also for a file without any non-blank line. An `"unknown"` entry has only `path`, `kind` and `lines`.

## Dock log entries (kind "dock")

A dock line matches `^(\d{2}:\d{2}:\d{2}\.\d{3}) (.*)$`: group 1 is its time, group 2 its message. Lines that do not match are ignored (they still count in `"lines"`). All rules below look at messages only. "starts with" is an exact, case-sensitive prefix test; "contains" is an exact substring test unless stated otherwise.

| key | value |
|---|---|
| `first_time`, `last_time` | the time of the first and of the last dock line (strings like `"11:00:40.463"`), `null` when there is none |
| `elevated_start` | `true` when a message starts with `started as administrator`, else `false` |
| `dock_counts` | a list with the number N of every message that is exactly `docks: N` (regex fullmatch `docks: (\d+)`), in order, as integers |
| `oled_on` | messages starting with `oled: on (` |
| `oled_off` | messages equal to `oled: off` |
| `tray_icons_added` | messages starting with `tray host: + ` |
| `tray_icons_shown` | of those `tray host: + ` messages, the ones that contain ` shown (` (with the spaces) and do not contain ` not shown (` |
| `ff_starts` | messages that fully match `fluentflyout: running \(process \d+\)` |
| `ff_stops` | messages equal to `fluentflyout: no longer running` |
| `ff_settings_reads` | messages starting with `fluentflyout: settings ` that also contain ` - widget ` |
| `ff_widget_moves` | messages starting with `fluentflyout: its widget goes to the dock on the ` |
| `ff_last_widget_monitor` | for the last of those messages: the word right after `dock on the ` (up to the next space) if that word is `main` or `other`, else `null`; `null` when there is no such message |
| `ff_widget_in_explorer` | messages starting with `fluentflyout: widget window is in ` that contain `Explorer` |
| `ff_widget_closed` | messages starting with `fluentflyout: its widget window closed` |
| `ff_widget_back` | messages starting with `fluentflyout: its new widget window is here after` |
| `ff_slot_open` | messages starting with `fluentflyout: widget slot open` |
| `ff_slot_closed` | messages starting with `fluentflyout: widget slot closed` |
| `ff_media_changes` | messages equal to `fluentflyout: media playing` or to `fluentflyout: nothing playing` |
| `problems` | the messages that contain, ignoring case, any of `failed`, `could not`, `cannot`, `unavailable`, `did not`, `has not`: the first 20 of them, in order, as strings (the message only, without the time) |

All counts are integers, 0 when nothing matches.

## FluentFlyout log entries (kind "fluentflyout")

A record line matches `^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\.\d+\|([A-Z]+)\|([^|]*)\|(.*)$`: date, time, level, logger name, message. Other lines (for example the lines of an exception's stack trace) are ignored; they still count in `"lines"`.

| key | value |
|---|---|
| `first_time`, `last_time` | `"<date> <time>"` of the first and of the last record, like `"2026-10-23 10:26:56"` (no fraction), `null` when there is none |
| `starts` | messages equal to `Starting FluentFlyout MainWindow` |
| `versions` | the distinct X of messages that fully match `Current version: (\S+)`, in order of first appearance |
| `newest_version` | group 1 of `re.search(r"Newest: ([^,\s]+)", message)` for the last message where it matches, else `null` |
| `saves` | messages starting with `Settings successfully saved` |
| `widget_recreated` | messages equal to `Taskbar Widget window recreated successfully` |
| `display_refreshes` | messages starting with `Refreshing window DPI and placement after display change` |
| `display_change_reasons` | an object: for each of those `display_refreshes` messages, the text inside its first pair of parentheses (`re.search(r"\(([^)]*)\)", message)` group 1) mapped to how often it occurs |
| `last_monitor_count` | the integer of `re.search(r"monitors=(\d+)", message)` for the last message where it matches, else `null` |
| `explorer_restarts` | messages starting with `Explorer restart detected` |
| `warnings` | records with level `WARN` |
| `errors` | records with level `ERROR` or `FATAL` |

## Example

For a dock.log holding only these two lines:

```
11:00:40.700 docks: 2
11:00:41.287 fluentflyout: running (process 6612)
```

the entry is

```json
{"path": "dock.log", "kind": "dock", "lines": 2, "first_time": "11:00:40.700", "last_time": "11:00:41.287",
 "elevated_start": false, "dock_counts": [2], "oled_on": 0, "oled_off": 0, "tray_icons_added": 0, "tray_icons_shown": 0,
 "ff_starts": 1, "ff_stops": 0, "ff_settings_reads": 0, "ff_widget_moves": 0, "ff_last_widget_monitor": null,
 "ff_widget_in_explorer": 0, "ff_widget_closed": 0, "ff_widget_back": 0, "ff_slot_open": 0, "ff_slot_closed": 0,
 "ff_media_changes": 0, "problems": []}
```

The check runs your tool on the sample files and on many generated logs (with blank lines, foreign lines, CRLF and LF line ends, with and without a byte order mark) and compares every key, so follow the rules above exactly and do not special-case the samples.
