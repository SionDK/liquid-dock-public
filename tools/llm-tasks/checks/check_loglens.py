#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Checks a tools/loglens.py candidate (task T5) against the spec in prompts/T5_loglens.md.

  python check_loglens.py CANDIDATE.py [--seed N] [--rounds N]

1. the sample logs in ../fixtures, against the expected summaries in expected/fixtures.json;
2. edge cases: empty and blank files, an unknown file, a missing file and a folder (exit code 1), no arguments (2);
3. generated logs: random sequences of real-looking messages, with blank and foreign lines, CRLF or LF and
   with or without a byte order mark. The generator knows what each message it writes adds to the summary,
   so every key is compared without a second implementation of the tool.
Exit code 0 when everything matches. The candidate runs as `python -I -X utf8 CANDIDATE FILE...`.
"""
import argparse
import json
import random
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = HERE.parent / "fixtures"
PROBLEM_WORDS = ["failed", "could not", "cannot", "unavailable", "did not", "has not"]

DOCK_KEYS = ["first_time", "last_time", "elevated_start", "dock_counts", "oled_on", "oled_off", "tray_icons_added",
             "tray_icons_shown", "ff_starts", "ff_stops", "ff_settings_reads", "ff_widget_moves", "ff_last_widget_monitor",
             "ff_widget_in_explorer", "ff_widget_closed", "ff_widget_back", "ff_slot_open", "ff_slot_closed",
             "ff_media_changes", "problems"]
FF_KEYS = ["first_time", "last_time", "starts", "versions", "newest_version", "saves", "widget_recreated",
           "display_refreshes", "display_change_reasons", "last_monitor_count", "explorer_restarts", "warnings", "errors"]


# ---------------------------------------------------------------- running the candidate

def run(candidate, paths, timeout=60):
    """(exit code, parsed JSON or None, a note on what went wrong)."""
    try:
        r = subprocess.run([sys.executable, "-I", "-X", "utf8", str(candidate), *[str(p) for p in paths]],
                           capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, None, f"it did not finish in {timeout} s"
    out = r.stdout.decode("utf-8", "replace")
    err = r.stderr.decode("utf-8", "replace")
    if not paths:
        return r.returncode, None, err
    try:
        data = json.loads(out)
    except ValueError:
        return r.returncode, None, ("standard output is not one JSON object. It starts with:\n" + out[:400]
                                    + ("\nstandard error:\n" + err[-1500:] if err.strip() else ""))
    return r.returncode, data, err


def short(v, n=160):
    s = json.dumps(v, ensure_ascii=False)
    return s if len(s) <= n else s[:n] + "..."


def compare(entry, expected, label):
    """Differences between one entry and the expected one, as lines."""
    if not isinstance(entry, dict):
        return [f"{label}: the entry is not an object: {short(entry)}"]
    out = []
    if "error" in expected:
        if set(entry) != {"path", "error"} or not isinstance(entry.get("error"), str) or not entry["error"]:
            out.append(f"{label}: an unreadable file gives exactly {{\"path\": ..., \"error\": \"<message>\"}}, got {short(entry)}")
        elif entry["path"] != expected["path"]:
            out.append(f"{label}: path {short(entry['path'])}, expected the argument {short(expected['path'])}")
        return out
    missing = [k for k in expected if k not in entry]
    extra = [k for k in entry if k not in expected]
    if missing:
        out.append(f"{label}: missing keys {missing}")
    if extra:
        out.append(f"{label}: keys that do not belong to kind {expected.get('kind')!r}: {extra}")
    for k, want in expected.items():
        if k in entry and entry[k] != want:
            got = entry[k]
            if isinstance(want, list) and isinstance(got, list):
                diff = next((i for i in range(max(len(want), len(got))) if i >= len(want) or i >= len(got) or want[i] != got[i]), None)
                out.append(f"{label}: {k}: expected {len(want)} items, got {len(got)}; first difference at item {diff}: "
                           f"expected {short(want[diff]) if diff is not None and diff < len(want) else '(none)'}, "
                           f"got {short(got[diff]) if diff is not None and diff < len(got) else '(none)'}")
            else:
                out.append(f"{label}: {k}: expected {short(want)}, got {short(got)}")
    return out


# ---------------------------------------------------------------- generated logs

def write_log(path, lines, rng):
    text = ("\r\n" if rng.random() < 0.5 else "\n").join(lines) + ("\r\n" if rng.random() < 0.5 else "")
    data = text.encode("utf-8")
    if rng.random() < 0.5:
        data = b"\xef\xbb\xbf" + data
    path.write_bytes(data)


def problem(msg):
    return any(w in msg.lower() for w in PROBLEM_WORDS)


class DockLog:
    """A dock.log built from real message shapes; `exp` is what the summary must say."""

    def __init__(self, rng):
        self.rng = rng
        self.lines = []
        self.ms = rng.randrange(0, 20 * 3600 * 1000)
        self.exp = {"kind": "dock", "lines": 0, "first_time": None, "last_time": None, "elevated_start": False,
                    "dock_counts": [], "oled_on": 0, "oled_off": 0, "tray_icons_added": 0, "tray_icons_shown": 0,
                    "ff_starts": 0, "ff_stops": 0, "ff_settings_reads": 0, "ff_widget_moves": 0,
                    "ff_last_widget_monitor": None, "ff_widget_in_explorer": 0, "ff_widget_closed": 0, "ff_widget_back": 0,
                    "ff_slot_open": 0, "ff_slot_closed": 0, "ff_media_changes": 0, "problems": []}

    def stamp(self):
        self.ms = (self.ms + self.rng.randrange(1, 90000)) % (24 * 3600 * 1000)
        s, ms = divmod(self.ms, 1000)
        return f"{s // 3600:02d}:{s // 60 % 60:02d}:{s % 60:02d}.{ms:03d}"

    def line(self, msg, is_problem=False, **inc):
        assert problem(msg) == is_problem, msg   # the templates below must say which messages are problems
        t = self.stamp()
        pad = self.rng.choice(["", "", "", " ", "\t", "  "])   # trailing whitespace is stripped
        self.lines.append(f"{t} {msg}{pad}")
        e = self.exp
        e["lines"] += 1
        if e["first_time"] is None:
            e["first_time"] = t
        e["last_time"] = t
        for k, v in inc.items():
            if k == "dock_count":
                e["dock_counts"].append(v)
            elif k == "last_monitor":
                e["ff_last_widget_monitor"] = v
            elif k == "elevated":
                e["elevated_start"] = True
            else:
                e[k] += v
        if is_problem and len(e["problems"]) < 20:
            e["problems"].append(msg)

    def foreign(self):
        r = self.rng
        text = r.choice(["   continued from the line above", "garbage without a time", "1:02:03.456 an hour with one digit",
                         "11:00:40.46 two digits of milliseconds", "11:00:40.4634 four digits of milliseconds",
                         "2026-10-23 11:00:40.4634|INFO|X|a FluentFlyout line inside dock.log", "[11:00:40.463] in brackets",
                         "11:00:40.463\tdocks: 2 (a tab, not a space)"])
        self.lines.append(text)
        self.exp["lines"] += 1

    def blank(self):
        self.lines.append(self.rng.choice(["", "   ", "\t"]))

    def event(self):
        r = self.rng
        exe = r.choice(["hwinfo64.exe", "kakaotalk.exe", "discord.exe", "steam.exe", "tailscale-ipn.exe", "onedrive.exe"])
        uid = r.choice([0, 1, 100, 222, 4294967294])
        pic = r.choice(["16x16", "32x32", "24x24", "unreadable", "empty", "not sent"])
        mon = r.choice(["main", "other"])
        pos = "0,0" if mon == "main" else f"{r.choice([-1440, 2560, 1920])},{r.choice([-360, 0, 120])}"
        n = r.randint(1, 3)
        choices = [
            (3, lambda: self.line(f"docks: {n}", dock_count=n)),
            (1, lambda: self.line(f"docks: {n} after a display change")),
            (1, lambda: self.line("started as administrator, running on as the user", elevated=True)),
            (1, lambda: self.line("started as administrator, running on as the user - still elevated (Explorer is)", elevated=True)),
            (2, lambda: self.line(f"oled: on ({r.choice(['one', 'all'])}, {n} monitor(s), wallpaper 1, icons {r.randint(0, 1)})", oled_on=1)),
            (2, lambda: self.line("oled: off", oled_off=1)),
            (1, lambda: self.line(f"oled: {n} cover(s)")),
            (1, lambda: self.line("oled: one more cover")),
            (4, lambda: self.tray(exe, uid, pic)),
            (1, lambda: self.line(f"tray host: now {exe} uid={uid} picture=ok shown (entry 17300437206130026798)")),
            (1, lambda: self.line(f"tray host:   candidate entry 8374702915217517684: c:\\program files\\{exe} uid={uid} promoted=0")),
            (1, lambda: self.line(f"tray host: {r.randint(2, 30)} icons from {r.randint(2, 20)} programs so far, {r.randint(0, 5)} shown on the dock")),
            (1, lambda: self.line(f"shell pop-up in front: dock {n} shown")),
            (2, lambda: self.line(f"fluentflyout: running (process {r.randint(100, 99999)})", ff_starts=1)),
            (1, lambda: self.line(f"fluentflyout: running (process {r.randint(100, 99999)}) again")),
            (2, lambda: self.line("fluentflyout: no longer running", ff_stops=1)),
            (1, lambda: self.line("fluentflyout: integration switched off")),
            (2, lambda: self.line("fluentflyout: settings C:\\Users\\USER\\AppData\\Roaming\\FluentFlyout\\settings.xml - widget 1, "
                                  f"hide when nothing plays {r.randint(0, 1)}, monitor {r.randint(0, 2)}, visualizer 1, app filter 0 (block), animation 2",
                                  ff_settings_reads=1)),
            (1, lambda: self.line("fluentflyout: settings file gone - its defaults assumed")),
            (3, lambda: self.line(f"fluentflyout: its widget goes to the dock on the {mon} monitor at {pos} (FluentFlyout's \"{n}\") - "
                                  f"{r.choice(['where FluentFlyout put it', 'from its settings'])}, its setting \"{n}\"",
                                  ff_widget_moves=1, last_monitor=mon)),
            (1, lambda: self.line("fluentflyout: its widget goes to the dock on the (no monitor) - not known yet",
                                  ff_widget_moves=1, last_monitor=None)),
            (2, lambda: self.line("fluentflyout: widget window is in " + r.choice(["a secondary Explorer taskbar (hidden by the dock)",
                                                                                   "Explorer's taskbar (hidden by the dock)"]),
                                  ff_widget_in_explorer=1)),
            (2, lambda: self.line("fluentflyout: widget window is in " + r.choice(["the dock's window", "a window of class WorkerW"]))),
            (1, lambda: self.line(f"fluentflyout: FluentFlyout took that taskbar: its widget belongs on the {mon} monitor at {pos} (FluentFlyout's \"{n}\")")),
            (2, lambda: self.line("fluentflyout: its widget window closed - keeping its place for the new one", ff_widget_closed=1)),
            (2, lambda: self.line(f"fluentflyout: its new widget window is here after {r.randint(1, 29) / 10:.1f} s", ff_widget_back=1)),
            (3, lambda: self.line("fluentflyout: widget slot open (showing)", ff_slot_open=1)),
            (3, lambda: self.line("fluentflyout: widget slot closed (" + r.choice(["nothing playing, hide completely", "empty",
                                                                                   "widget hidden or not in the dock's window"]) + ")",
                                  ff_slot_closed=1)),
            (3, lambda: self.line(r.choice(["fluentflyout: media playing", "fluentflyout: nothing playing"]), ff_media_changes=1)),
            (1, lambda: self.line("fluentflyout: media playing again")),
            (1, lambda: self.line("fluentflyout: asked FluentFlyout to open its settings")),
            (1, lambda: self.line("tray host: the dock had come over the widget - back on top")),
            (1, lambda: self.line(r.choice(["GPU: NVIDIA GeForce RTX 4080 SUPER", "capture mode: Windows.Graphics.Capture",
                                            "borderless capture access: 4", "logon task rc=0", "tray host: down"]))),
            # problems
            (1, lambda: self.line("fluentflyout: its widget window did not come back", True)),
            (1, lambda: self.line("fluentflyout: media sessions unavailable - the widget's slot stays open", True)),
            (1, lambda: self.line(f"tray host: window failed {r.randint(1, 1500)}", True)),
            (1, lambda: self.line("tray host: cannot take messages from programs that are not elevated (5): not started", True)),
            (1, lambda: self.line(f"fluentflyout: could not start C:\\Program Files\\FluentFlyout\\FluentFlyout.exe to open its settings", True)),
            (1, lambda: self.line(f"tray host: {exe} is running and shown on the taskbar, but its icon has not come; if it is missing "
                                  "on the dock, restart that program", True)),
            (1, lambda: self.line("fluentflyout: FluentFlyout has not moved its widget into the dock's window (it is in Explorer's taskbar "
                                  "(hidden by the dock)) - is FluentFlyout's taskbar widget on, with Premium?", True)),
            (1, lambda: self.line(f"gfx: device {r.choice(['FAILED', 'Failed'])} (887a0005)", True)),
            (1, lambda: self.line("oled: wallpaper api Unavailable 80004005", True)),
            (1, lambda: self.line("tray host: did not stop in time", True)),
        ]
        total = sum(w for w, _ in choices)
        pick = r.uniform(0, total)
        for w, f in choices:
            pick -= w
            if pick <= 0:
                f()
                return
        choices[-1][1]()

    def tray(self, exe, uid, pic):
        r = self.rng
        status = r.choice(["shown", "not shown", "hidden by the program"])
        how = r.choice(["+", "+", "+ (by a modify)"])
        flags = r.choice(["", " guid", " guid no-callback", " no-callback"])
        why = r.choice(["entry 2001972136577579180", "no settings entry yet", "no NotifyIconSettings: all shown"])
        self.line(f"tray host: {how} {exe} uid={uid}{flags} picture={pic} {status} ({why})",
                  tray_icons_added=1, tray_icons_shown=1 if status == "shown" else 0)

    def build(self, events):
        self.line("GPU: NVIDIA GeForce RTX 4080 SUPER")   # a dock line first: the kind
        for _ in range(events):
            x = self.rng.random()
            if x < 0.06:
                self.blank()
            elif x < 0.10:
                self.foreign()
            else:
                self.event()
        return self


class FluentLog:
    """A FluentFlyout log (NLog layout) built from real message shapes; `exp` is what the summary must say."""

    LOGGERS = ["FluentFlyoutWPF.MainWindow", "FluentFlyout.Classes.Settings.SettingsManager",
               "FluentFlyoutWPF.Classes.Services.UpdateCheckerService", "FluentFlyoutWPF.Windows.VolumeMixerWindow"]

    def __init__(self, rng):
        self.rng = rng
        self.lines = []
        self.t = rng.randrange(0, 20 * 3600)
        self.day = rng.randint(1, 28)
        self.exp = {"kind": "fluentflyout", "lines": 0, "first_time": None, "last_time": None, "starts": 0, "versions": [],
                    "newest_version": None, "saves": 0, "widget_recreated": 0, "display_refreshes": 0,
                    "display_change_reasons": {}, "last_monitor_count": None, "explorer_restarts": 0, "warnings": 0, "errors": 0}

    def rec(self, msg, level="INFO", **inc):
        r = self.rng
        self.t = min(self.t + r.randrange(0, 900), 24 * 3600 - 1)
        stamp = f"2026-10-{self.day:02d} {self.t // 3600:02d}:{self.t // 60 % 60:02d}:{self.t % 60:02d}"
        frac = f"{r.randrange(0, 10000):04d}"
        self.lines.append(f"{stamp}.{frac}|{level}|{r.choice(self.LOGGERS)}|{msg}" + r.choice(["", "", " ", "\t"]))
        e = self.exp
        e["lines"] += 1
        if e["first_time"] is None:
            e["first_time"] = stamp
        e["last_time"] = stamp
        if level == "WARN":
            e["warnings"] += 1
        if level in ("ERROR", "FATAL"):
            e["errors"] += 1
        for k, v in inc.items():
            if k == "version":
                if v not in e["versions"]:
                    e["versions"].append(v)
            elif k == "newest":
                e["newest_version"] = v
            elif k == "monitors":
                e["last_monitor_count"] = v
            elif k == "reason":
                e["display_change_reasons"][v] = e["display_change_reasons"].get(v, 0) + 1
            else:
                e[k] += v

    def extra(self, text):
        self.lines.append(text)
        self.exp["lines"] += 1

    def event(self):
        r = self.rng
        n = r.randint(1, 3)
        ver = r.choice(["v2.15.0", "v2.16.0", "v2.14.2"])
        reason = r.choice(["WM_DISPLAYCHANGE", "WM_SETTINGCHANGE/SPI_SETWORKAREA", "WM_DPICHANGED"])
        choices = [
            (2, lambda: self.rec("Starting FluentFlyout MainWindow", starts=1)),
            (1, lambda: self.rec("Starting FluentFlyout MainWindow again")),
            (2, lambda: self.rec(f"Current version: {ver}", version=ver)),
            (1, lambda: self.rec(f"Current version: {ver} (portable)")),
            (2, lambda: self.rec(f"Update check complete. Current: {ver}, Newest: v2.16.{n}, Update available: True", newest=f"v2.16.{n}")),
            (1, lambda: self.rec("Settings successfully restored")),
            (3, lambda: self.rec("Settings successfully saved to C:\\Users\\USER\\AppData\\Roaming\\FluentFlyout\\settings.xml", saves=1)),
            (1, lambda: self.rec("Settings successfully saved to C:\\odd|path\\settings.xml", saves=1)),
            (1, lambda: self.rec("License synced on startup - Store: False, Premium: True")),
            (2, lambda: self.rec("Recreating Taskbar Widget window")),
            (3, lambda: self.rec("Taskbar Widget window recreated successfully", widget_recreated=1)),
            (3, lambda: self.rec(f"Refreshing window DPI and placement after display change ({reason}); monitors={n}",
                                 display_refreshes=1, reason=reason, monitors=n)),
            (1, lambda: self.rec(f"Monitor list changed; monitors={n + 1}", monitors=n + 1)),
            (2, lambda: self.rec("Explorer restart detected (TaskbarCreated)", "WARN", explorer_restarts=1)),
            (1, lambda: self.rec("Explorer stabilized, resuming taskbar integration")),
            (2, lambda: self.rec("Did not try to restore OSD because it was either not found or was not hidden.", "WARN")),
            (1, lambda: self.rec("Detailed media session state", r.choice(["DEBUG", "TRACE"]))),
            (1, lambda: self.error()),
        ]
        total = sum(w for w, _ in choices)
        pick = r.uniform(0, total)
        for w, f in choices:
            pick -= w
            if pick <= 0:
                f()
                return
        choices[-1][1]()

    def error(self):
        r = self.rng
        self.rec("Failed to update the taskbar widget", r.choice(["ERROR", "FATAL"]))
        for _ in range(r.randint(1, 4)):
            self.extra(r.choice(["System.InvalidOperationException: Operation is not valid due to the current state of the object.",
                                 "   at FluentFlyoutWPF.MainWindow.UpdateTaskbar()", "   at System.Windows.Threading.Dispatcher.Invoke(Action)"]))

    def build(self, events):
        self.rec("Starting FluentFlyout MainWindow", starts=1)   # a record first: the kind
        for _ in range(events):
            if self.rng.random() < 0.06:
                self.lines.append(self.rng.choice(["", "  "]))
            else:
                self.event()
        return self


# ---------------------------------------------------------------- the checks

def check_fixtures(candidate):
    expected = json.loads((HERE / "expected" / "fixtures.json").read_text(encoding="utf-8"))
    names = list(expected)
    paths = [FIXTURES / n for n in names]
    rc, data, note = run(candidate, paths)
    if data is None:
        return [f"sample logs: {note}"]
    problems = []
    if rc != 0:
        problems.append(f"sample logs: exit code {rc}, expected 0")
    files = data.get("files") if isinstance(data, dict) else None
    if not isinstance(files, list) or len(files) != len(names):
        return problems + [f"sample logs: expected {{\"files\": [...]}} with {len(names)} entries, got {short(data, 300)}"]
    for name, path, entry in zip(names, paths, files):
        want = dict(expected[name], path=str(path))
        problems += compare(entry, want, name)
    return problems


def check_edges(candidate, tmp):
    problems = []
    rc, _, _ = run(candidate, [])
    if rc != 2:
        problems.append(f"no arguments: exit code {rc}, expected 2 (and a usage message on standard error)")
    cases = {
        "empty.log": (b"", {"kind": "unknown", "lines": 0}),
        "blank.log": (b"  \r\n\t\n\n", {"kind": "unknown", "lines": 0}),
        "bom_only.log": (b"\xef\xbb\xbf", {"kind": "unknown", "lines": 0}),
        "other.txt": (b"hello\n11:00:40.700 docks: 2\n", {"kind": "unknown", "lines": 2}),
        "late_dock.log": (b"\n\n   \n11:00:40.700 docks: 2\n", None),
        "bad_utf8.log": (b"11:00:40.700 oled: off\n11:00:41.000 tray host: + \xff\xfe.exe uid=1 picture=16x16 shown (x)\n", None),
        "many_problems.log": ("".join(f"11:00:{i:02d}.000 tray host: window failed {i}\r\n" for i in range(25)).encode(),
                              dict(DockLog(random.Random(0)).exp, lines=25, first_time="11:00:00.000", last_time="11:00:24.000",
                                   problems=[f"tray host: window failed {i}" for i in range(20)])),
    }
    paths = []
    for name, (data, _) in cases.items():
        (tmp / name).write_bytes(data)
        paths.append(tmp / name)
    rc, data, note = run(candidate, paths)
    if data is None:
        return problems + [f"edge cases: {note}"]
    files = data.get("files") if isinstance(data, dict) else None
    if not isinstance(files, list) or len(files) != len(paths):
        return problems + [f"edge cases: expected {len(paths)} entries, got {short(data, 300)}"]
    if rc != 0:
        problems.append(f"edge cases (all readable): exit code {rc}, expected 0")
    for (name, (_, want)), path, entry in zip(cases.items(), paths, files):
        if want is not None:
            problems += compare(entry, dict(want, path=str(path)), name)
    late = files[4] if isinstance(files[4], dict) else {}
    if late.get("kind") != "dock" or late.get("lines") != 1 or late.get("dock_counts") != [2]:
        problems.append(f"late_dock.log (blank lines, then one dock line): expected kind dock, lines 1, dock_counts [2]; got "
                        f"kind {short(late.get('kind'))}, lines {short(late.get('lines'))}, dock_counts {short(late.get('dock_counts'))}")
    bad = files[5] if isinstance(files[5], dict) else {}
    if bad.get("kind") != "dock" or bad.get("oled_off") != 1 or bad.get("tray_icons_shown") != 1:
        problems.append("bad_utf8.log (invalid UTF-8 bytes in a line): decode with errors=\"replace\"; expected oled_off 1 and "
                        f"tray_icons_shown 1, got {short({k: bad.get(k) for k in ('kind', 'oled_off', 'tray_icons_shown')})}")
    missing = tmp / "does_not_exist.log"
    rc, data, note = run(candidate, [paths[3], missing, tmp])
    if data is None:
        problems.append(f"unreadable files: {note}")
    else:
        files = data.get("files") if isinstance(data, dict) else None
        if rc != 1:
            problems.append(f"a missing file and a folder among the arguments: exit code {rc}, expected 1")
        if not isinstance(files, list) or len(files) != 3:
            problems.append(f"unreadable files: expected 3 entries, got {short(data, 300)}")
        else:
            problems += compare(files[0], {"path": str(paths[3]), "kind": "unknown", "lines": 2}, "other.txt (with unreadable files)")
            problems += compare(files[1], {"path": str(missing), "error": ""}, "a missing file")
            problems += compare(files[2], {"path": str(tmp), "error": ""}, "a folder")
    return problems


def check_generated(candidate, tmp, seed, rounds):
    rng = random.Random(seed)
    problems = []
    for i in range(rounds):
        dock = DockLog(rng).build(rng.randint(20, 160))
        flu = FluentLog(rng).build(rng.randint(10, 80))
        dp, fp = tmp / f"gen{i}_dock.log", tmp / f"gen{i}_fluentflyout.txt"
        write_log(dp, dock.lines, rng)
        write_log(fp, flu.lines, rng)
        rc, data, note = run(candidate, [dp, fp])
        if data is None:
            problems.append(f"generated logs {i}: {note}")
            break
        files = data.get("files") if isinstance(data, dict) else None
        if not isinstance(files, list) or len(files) != 2:
            problems.append(f"generated logs {i}: expected 2 entries, got {short(data, 300)}")
            break
        if rc != 0:
            problems.append(f"generated logs {i}: exit code {rc}, expected 0")
        problems += compare(files[0], dict(dock.exp, path=str(dp)), f"generated dock log {i}")
        problems += compare(files[1], dict(flu.exp, path=str(fp)), f"generated FluentFlyout log {i}")
        if len(problems) > 12:
            break
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("candidate")
    ap.add_argument("--seed", type=int, default=None, help="the generated logs' seed (default: from the clock)")
    ap.add_argument("--rounds", type=int, default=8)
    ap.add_argument("--keep", help="keep the generated logs in this folder")
    args = ap.parse_args()
    seed = args.seed if args.seed is not None else int(time.time()) % 100000
    candidate = Path(args.candidate).resolve()
    if not candidate.exists():
        print(f"no such file: {candidate}")
        return 2
    with tempfile.TemporaryDirectory(prefix="loglens-check-") as t:
        tmp = Path(args.keep).resolve() if args.keep else Path(t)
        tmp.mkdir(parents=True, exist_ok=True)
        results = [("sample logs", check_fixtures(candidate)),
                   ("edge cases", check_edges(candidate, tmp)),
                   (f"generated logs (seed {seed})", check_generated(candidate, tmp, seed, args.rounds))]
    failed = 0
    for name, problems in results:
        if problems:
            failed += 1
            print(f"FAIL {name}:")
            for p in problems[:12]:
                print("  - " + p)
            if len(problems) > 12:
                print(f"  ... and {len(problems) - 12} more")
        else:
            print(f"PASS {name}")
    print("loglens check: " + ("ALL PASSED" if not failed else f"{failed} of {len(results)} parts failed (seed {seed})"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
