#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""The log-message guide (task T6): the messages it must explain, and the check of a written guide.

  python check_log_guide.py --list      the messages, each with the code right before it (the task's context)
  python check_log_guide.py GUIDE.md    checks a guide: every message key in a table row with a Korean explanation,
                                        no invented keys, mostly Korean text, the required sections

The keys come from the Log(L"...") calls in src/fluent_*.cpp and src/tray_host*.cpp: the format string
with every printf field (%s, %d, %.1f ...) written as "…". "fluentflyout: %s" calls are expanded into the
texts they are given (the two literals of a ?: expression, or the reason passed to Forget(L"...")).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCES = ["src/fluent_flyout.cpp", "src/fluent_settings_file.cpp", "src/fluent_media.cpp", "src/fluent_placement.cpp", "src/tray_host.cpp", "src/tray_host_icons.cpp", "src/tray_host_settings.cpp", "src/tray_host_widget.cpp"]
FIELD = re.compile(r"%[-+ #0]*(?:\d+|\*)?(?:\.\d+)?(?:hh|h|ll|l|L|z|j|t|I64|I32|I)?[diouxXeEfgGaAcspn]")
SECTIONS = ["## FluentFlyout 연동", "## 트레이 아이콘", "## 문제를 알릴 때"]


def read_literals(src, j):
    """Adjacent C++ string literals (L"a" L"b" ...) starting at or after src[j]: (text, index after them, found)."""
    parts, found = [], False
    while True:
        k = j
        while k < len(src) and src[k] in " \t\r\n":
            k += 1
        if src.startswith('L"', k):
            k += 2
        elif src.startswith('"', k):
            k += 1
        else:
            return "".join(parts), j, found
        buf = []
        while k < len(src) and src[k] != '"':
            if src[k] == "\\" and k + 1 < len(src):
                buf.append({"n": "\n", "t": "\t", "r": "\r", "0": "\0"}.get(src[k + 1], src[k + 1]))
                k += 2
            else:
                buf.append(src[k])
                k += 1
        parts.append("".join(buf))
        j, found = k + 1, True


def call_end(src, j):
    """The index of the ')' closing a call whose arguments continue at src[j] (one '(' open)."""
    depth = 1
    while j < len(src):
        c = src[j]
        if c in "\"'":
            q = c
            j += 1
            while j < len(src) and src[j] != q:
                j += 2 if src[j] == "\\" else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return len(src)


def key_of(fmt):
    k = FIELD.sub("\x00", fmt.replace("%%", "\x01"))
    return re.sub("\x00+", "…", k).replace("\x01", "%")


def code_before(src, pos, end, before=4):
    """The source lines just before `pos` and those of the call itself (up to `end`), dedented."""
    first_line = src.count("\n", 0, pos)
    last_line = src.count("\n", 0, end)
    lines = src.split("\n")
    lo = max(0, first_line - before)
    chunk = [l.rstrip() for l in lines[lo:last_line + 1]]
    while chunk and not chunk[0].strip():
        chunk.pop(0)
    indent = min((len(l) - len(l.lstrip()) for l in chunk if l.strip()), default=0)
    return first_line + 1, [l[indent:] for l in chunk][-10:]


def messages():
    """[(key, source file, line number, code lines)] in source order, each key once."""
    out, seen = [], set()

    def add(key, rel, line, code):
        if key not in seen:
            seen.add(key)
            out.append((key, rel, line, code))

    for rel in SOURCES:
        src = (ROOT / rel).read_text(encoding="utf-8")
        for m in re.finditer(r"\bLog\(", src):
            fmt, j, found = read_literals(src, m.end())
            if not found:
                continue
            end = call_end(src, j)
            line, code = code_before(src, m.start(), end)
            prefix = re.match(r"^([a-z ]+): %s$", fmt)
            if prefix:   # "fluentflyout: %s": the texts it is given
                alternatives = [t for t in re.findall(r'L"((?:[^"\\]|\\.)*)"', src[j:end])]
                for t in alternatives:
                    add(f"{prefix.group(1)}: {t}", rel, line, code)
                continue
            add(key_of(fmt), rel, line, code)
        if rel.endswith("fluent_flyout.cpp"):   # Forget(L"why") logs "fluentflyout: why"
            for m in re.finditer(r"\bForget\(L\"((?:[^\"\\]|\\.)*)\"\)", src):
                line, code = code_before(src, m.start(), m.end(), before=3)
                add(f"fluentflyout: {m.group(1)}", rel, line, code)
    return out


def hangul(text):
    return sum(1 for ch in text if "가" <= ch <= "힣")


def latin(text):
    return sum(1 for ch in text if ch.isascii() and ch.isalpha())


def check(path):
    doc = Path(path).read_text(encoding="utf-8-sig", errors="replace")
    keys = [k for k, *_ in messages()]
    problems = []
    lines = doc.splitlines()
    first = next((l for l in lines if l.strip()), "")
    if not first.startswith("# "):
        problems.append(f"the guide must start with a title line '# ...', it starts with: {first[:80]!r}")
    for s in SECTIONS:
        if not any(l.strip().startswith(s) for l in lines):
            problems.append(f"missing section heading: {s}")
    missing, unexplained = [], []
    for k in keys:
        span = f"`{k}`"
        rows = [l for l in lines if span in l]
        if not rows:
            missing.append(k)
            continue
        good = [l for l in rows if l.lstrip().startswith("|") and l.count("|") >= 4
                and hangul(re.sub(r"`[^`]*`", "", l)) >= 8]
        if not good:
            unexplained.append(k)
    if missing:
        problems.append(f"{len(missing)} of {len(keys)} message keys are not in the guide (each exactly as listed, inside backticks):\n"
                        + "\n".join(f"      `{k}`" for k in missing[:20]) + ("\n      ..." if len(missing) > 20 else ""))
    if unexplained:
        problems.append("these keys are not in a table row '| `key` | 뜻 | 할 일 |' with a Korean explanation:\n"
                        + "\n".join(f"      `{k}`" for k in unexplained[:20]))
    known = set(keys)
    invented = sorted({s for s in re.findall(r"`([^`\n]+)`", doc)
                       if re.match(r"\s*(fluentflyout|tray host):", s) and s not in known})
    if invented:
        problems.append("backticked messages that are not in the list (write each key exactly as listed, and no other "
                        "fluentflyout:/tray host: text in backticks):\n" + "\n".join(f"      `{s}`" for s in invented[:15]))
    prose = re.sub(r"```.*?```", "", doc, flags=re.S)
    prose = re.sub(r"`[^`\n]*`", "", prose)
    h, a = hangul(prose), latin(prose)
    if h < 300 or h < (h + a) * 0.5:
        problems.append(f"the explanations must be in Korean: {h} Hangul syllables and {a} Latin letters outside code")
    if any(l.lstrip().startswith("|") and l.count("|") >= 4 and l.count("`") % 2 for l in lines):
        problems.append("a table row has an unpaired backtick (each key goes inside one pair of backticks)")
    return problems, len(keys)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "--list":
        current = None
        for key, rel, line, code in messages():
            if rel != current:
                current = rel
                print(f"===== {rel}\n")
            print(f"KEY: {key}\n  at {rel}:{line}")
            for l in code:
                print("  | " + l)
            print()
        return 0
    problems, count = check(sys.argv[1])
    for p in problems:
        print("FAIL " + p)
    print(f"log guide check: {'ALL PASSED' if not problems else f'{len(problems)} problem(s)'} ({count} message keys)")
    return 1 if problems else 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")   # the list holds "…" and Korean
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
