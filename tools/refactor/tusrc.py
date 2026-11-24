#!/usr/bin/env python3
"""A test's view of src\\ before and after a test-only change: the preprocessed src\\ tokens of two copies of a test.

  python tools\\refactor\\tusrc.py --old-root build\\refactor-scratch\\2a\\before tests\\dock_layout_test.cpp tests\\x_test.cpp
  python tools\\refactor\\tusrc.py --old-root OLD --rename SHAppBarMessage=FakeAppBar tests\\dock_interaction_test.cpp
  python tools\\refactor\\tusrc.py --self-test tests\\dock_layout_test.cpp

What for: a test's fakes are text substitutions (#define RealName FakeName) that act only inside the src\\ files the test
includes after them. When fakes move into a shared header, a lost #define still compiles, and a real call on a hidden
window may give the same result, so the check counts stay the same. This tool shows that the test still sees the same
src\\ code: both copies of the test are preprocessed with cl /E (with #line), with the same options, and the tokens of
the parts whose #line names a file under <root>\\src (.cpp, .h) are compared in order, with the file each came from.
Whitespace and blank lines do not count; the test's own code and the Windows/C++ headers do not count.

  --old-root DIR   a tree with the test as it was: DIR\\tests\\... and DIR\\src\\... (copy tests\\, src\\, and what the
                   tests include, e.g. sensors\\, before the change). The test path is the same relative path in both.
  --new-root DIR   the tree as it is now (default: the repository)
  --cl-args "..."  cl options (default: tests\\run_regressions.bat's CF without /Fo: the dock tests' compile line)
  --rename A=B     an intended renaming: the old side's token A counts as B (e.g. a new fake on a src call). The
                   report says how many tokens it renamed, so the change is shown, not hidden. Repeatable.
  --dirs D ...     the folders of the root whose files are compared (default: src sensors installer: the helper's
                   and the installer's files, which some tests include, count as src too). It takes every word
                   after it: put the tests before it (tusrc.py --old-root OLD tests\\x_test.cpp --dirs src)
  --ignore-file    compare the tokens in order but not the file each came from: for code moved to another src file
                   at the same place in the include order (e.g. a part split off and included right where it was)
  --multiset       compare only how many times each token occurs, not their order nor their files: for a function
                   moved to another src file that the test includes elsewhere (its tokens then come at another place).
                   Weaker: two tokens swapped still count as the same; the report says which tokens differ in number
  --keep DIR       write each side's token list there (one "file:line token" per line) to read a difference
  --self-test T    preprocesses the test T twice (must be 'same'), then a copy of the tree (in build\\refactor-scratch\\
                   tusrc-selftest) with its first fake #define line removed (must be 'different'). Exit 0 when both hold.

Result per test: 'TUSRC: same <test> (<n> src tokens in <files> files[, renamed ...])' or 'TUSRC: DIFFERENT <test> ...'
with the first difference (file:line and tokens on both sides) and how many places differ. Exit code: 0 all the same,
1 a difference, 2 a preprocessing error. Needs the MSVC tools (tests\\vsenv.bat is called).

Limits: tokens are compared, not meaning (a fake that expands to the same tokens as before counts as the same; a src
file that is not included at all is not seen); macros the test defines but src never uses are invisible here (they
change nothing in src). Only tokens from the --dirs folders count: the bodies of the test's own fakes and stubs (in
tests\\, e.g. a shared fixture header) are not compared, so a fake whose body changed but whose name did not is the same. The tokenizer is simple (identifiers, numbers, string and character literals including raw
strings, every other character one token), which is enough for an equality check.
"""
import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRATCH = ROOT / "build" / "refactor-scratch"

LINE_DIRECTIVE = re.compile(r'^\s*#\s*(?:line\s+)?(\d+)\s+"((?:[^"\\]|\\.)*)"')
TOKEN = re.compile(r'''
    (?P<raw>(?:u8|u|U|L)?R"(?P<delim>[^()\\\s"]{0,16})\()      # raw string start: the rest is found by hand
  | (?:u8|u|U|L)?"(?:\\.|[^"\\\n])*"                             # string literal
  | (?:u8|u|U|L)?'(?:\\.|[^'\\\n])*'                             # character literal
  | [A-Za-z_][A-Za-z_0-9]*                                       # identifier, keyword
  | \.?[0-9](?:[eEpP][+-]|[A-Za-z0-9_.'])*                      # pp-number
  | \S                                                           # any other character
''', re.X)


def default_cl_args():
    bat = (ROOT / "tests" / "run_regressions.bat").read_text(encoding="utf-8", errors="replace")
    m = re.search(r'^set "CF=([^"]*)"', bat, re.M)
    if not m:
        raise SystemExit("tusrc: tests\\run_regressions.bat has no 'set \"CF=...\"' line (give --cl-args)")
    return " ".join(a for a in m.group(1).split() if not a.lower().startswith("/fo"))


def preprocess(root: Path, test: str, cl_args: str) -> str:
    src = (root / test).resolve()
    if not src.is_file():
        raise RuntimeError(f"no such file: {src}")
    # cl /E writes the preprocessed text to stdout, the file names it reads to stderr; /utf-8 keeps it UTF-8
    cmd = f'cmd /d /s /c "call "{ROOT}\\tests\\vsenv.bat" >nul 2>nul && cl {cl_args} /E "{src}""'
    r = subprocess.run(cmd, cwd=str(root), capture_output=True)
    if r.returncode != 0:
        err = r.stderr.decode("utf-8", "replace") + r.stdout.decode("utf-8", "replace")[-3000:]
        raise RuntimeError(f"cl /E failed for {src} (exit {r.returncode}):\n" + "\n".join(err.splitlines()[-30:]))
    return r.stdout.decode("utf-8", "replace")


def norm(p: str) -> str:
    return os.path.normcase(os.path.normpath(p.replace("\\\\", "\\")))


def tokens_of(text: str, root: Path, dirs):
    """[(file, line, token)] of the lines whose #line names a file in root\\<dir> (.cpp/.h), in order."""
    bases = [norm(str((root / d).resolve())) + os.sep for d in dirs]
    rootn = norm(str(root.resolve())) + os.sep
    out = []
    cur = None          # the relative name of the current src file, or None outside src
    line = 0
    chunk, chunk_lines = [], []
    cache = {}

    def flush():
        if cur is None or not chunk:
            chunk.clear(); chunk_lines.clear()
            return
        # join the file's lines with their numbers, tokenize, map each token back to its line
        text = "\n".join(chunk)
        starts, pos = [], 0
        for l in chunk:
            starts.append(pos); pos += len(l) + 1
        import bisect
        i = 0
        while i < len(text):
            m = TOKEN.match(text, i)
            if not m:
                i += 1
                continue
            if m.group().isspace():
                i = m.end(); continue
            if m.group("raw") is not None:
                end = text.find(")" + m.group("delim") + '"', m.end())
                end = len(text) if end < 0 else end + len(m.group("delim")) + 2
                tok = text[m.start():end]
                nxt = end
            else:
                tok = m.group()
                nxt = m.end()
            ln = chunk_lines[bisect.bisect_right(starts, m.start()) - 1]
            out.append((cur, ln, tok))
            i = nxt
        chunk.clear(); chunk_lines.clear()

    for raw in text.split("\n"):
        raw = raw.rstrip("\r")
        m = LINE_DIRECTIVE.match(raw)
        if m:
            flush()
            line = int(m.group(1))
            path = m.group(2)
            if path not in cache:
                n = norm(path)
                name = None
                for b in bases:
                    if n.startswith(b) and n.endswith((".cpp", ".h")):
                        name = n[len(rootn):].replace(os.sep, "/")
                        break
                cache[path] = name
            cur = cache[path]
            continue
        if cur is not None and raw.strip():
            chunk.append(raw); chunk_lines.append(line)
        line += 1
    flush()
    return out


def compare(test, old, new, renames, keep: Path | None, label_old="old", label_new="new", mode="file"):
    """mode: 'file' (tokens in order with their files), 'ignore-file' (in order, files not compared) or 'multiset'
    (how many of each token only)."""
    renamed = {}
    old_keys = []
    for f, _, t in old:
        if t in renames:
            renamed[t] = renamed.get(t, 0) + 1
            t = renames[t]
        old_keys.append((f, t))
    new_keys = [(f, t) for f, _, t in new]
    if keep:
        keep.mkdir(parents=True, exist_ok=True)
        stem = Path(test).stem
        for side, lst in ((label_old, old), (label_new, new)):
            with open(keep / f"{stem}.{side}.tokens.txt", "w", encoding="utf-8") as fh:
                for f, ln, t in lst:
                    fh.write(f"{f}:{ln} {t}\n")
    files = len({f for f, _ in new_keys})
    extra = ""
    if renamed:
        extra = ", renamed " + ", ".join(f"{a}->{renames[a]} x{n}" for a, n in sorted(renamed.items()))
    unused = [a for a in renames if a not in renamed]
    if unused:
        extra += ", rename never used: " + ", ".join(unused)
    if mode != "file":
        old_files = Counter(f for f, _ in old_keys)
        new_files = Counter(f for f, _ in new_keys)
        if old_files != new_files:
            moved = sorted(set(old_files) | set(new_files))
            extra += ", tokens per file: " + ", ".join(f"{f} {old_files.get(f, 0)}->{new_files.get(f, 0)}" for f in moved
                                                      if old_files.get(f, 0) != new_files.get(f, 0))
        extra += f", --{mode}"
        old_keys = [("", t) for _, t in old_keys]
        new_keys = [("", t) for _, t in new_keys]
    if mode == "multiset":
        ca, cb = Counter(t for _, t in old_keys), Counter(t for _, t in new_keys)
        if ca == cb:
            print(f"TUSRC: same {test} ({len(new_keys)} src tokens in {files} files{extra})")
            return True
        only_old, only_new = ca - cb, cb - ca
        print(f"TUSRC: DIFFERENT {test} (old {len(old_keys)}, new {len(new_keys)} src tokens; "
              f"{sum(only_old.values())} only old, {sum(only_new.values())} only new{extra})")
        for label, c in (("only old", only_old), ("only new", only_new)):
            if c:
                print(f"  {label}: " + ", ".join(f"{t} x{n}" for t, n in c.most_common(16))
                      + (f" ... {len(c) - 16} more" if len(c) > 16 else ""))
        return False
    if old_keys == new_keys:
        print(f"TUSRC: same {test} ({len(new_keys)} src tokens in {files} files{extra})")
        return True
    first = next((i for i, (a, b) in enumerate(zip(old_keys, new_keys)) if a != b), min(len(old_keys), len(new_keys)))
    sm = difflib.SequenceMatcher(None, old_keys, new_keys, autojunk=False)
    ops = [op for op in sm.get_opcodes() if op[0] != "equal"]
    print(f"TUSRC: DIFFERENT {test} (old {len(old_keys)}, new {len(new_keys)} src tokens; {len(ops)} places{extra})")
    for tag, i1, i2, j1, j2 in ops[:8]:
        def show(lst, a, b):
            if a >= len(lst):
                return "(end)"
            f, ln, _ = lst[a]
            toks = " ".join(t for _, _, t in lst[a:min(b, a + 12)])
            return f"{f}:{ln} [{toks}{' ...' if b - a > 12 else ''}]"
        print(f"  {tag}: old {show(old, i1, i2)}  new {show(new, j1, j2)}")
    if len(ops) > 8:
        print(f"  ... {len(ops) - 8} more")
    if first < len(old):
        print(f"  first difference at token {first}: old {old[first][0]}:{old[first][1]}")
    return False


def run(tests, old_root: Path, new_root: Path, cl_args, renames, dirs, keep, mode="file"):
    ok_all = True
    for test in tests:
        test = test.replace("/", "\\")
        try:
            a = tokens_of(preprocess(old_root, test, cl_args), old_root, dirs)
            b = tokens_of(preprocess(new_root, test, cl_args), new_root, dirs)
        except RuntimeError as e:
            print(f"TUSRC: ERROR {test}: {e}")
            return 2
        if not a:
            print(f"TUSRC: ERROR {test}: no src tokens on the old side (does it include src\\?)")
            return 2
        ok_all &= compare(test, a, b, renames, keep, mode=mode)
    return 0 if ok_all else 1


def self_test(test, cl_args, dirs):
    test = test.replace("/", "\\")
    print(f"tusrc self-test on {test}")
    a = tokens_of(preprocess(ROOT, test, cl_args), ROOT, dirs)
    same = compare(test, a, tokens_of(preprocess(ROOT, test, cl_args), ROOT, dirs), {}, None)
    # a copy of the tree with the test's first fake #define (one a src file then sees) removed
    tmp = SCRATCH / "tusrc-selftest"
    if tmp.exists():
        shutil.rmtree(tmp)
    for d in ["tests"] + list(dirs) + [d for d in ("sensors", "installer") if d not in dirs]:
        if (ROOT / d).is_dir():
            shutil.copytree(ROOT / d, tmp / d)
    path = tmp / test
    lines = path.read_text(encoding="utf-8").split("\n")
    drop = None
    for i, l in enumerate(lines):
        m = re.match(r"\s*#define\s+(\w+)\s+(\w+)\s*$", l)
        if m and m.group(1) != "private" and re.match(r"(Matrix|Hunt|Fake)", m.group(2)):
            drop = i
            break
    if drop is None:
        print("self-test: no '#define RealName Matrix*/Hunt*/Fake*' line in the test")
        return 2
    print(f"  removed line {drop + 1}: {lines[drop].strip()}")
    lines[drop] = ""
    path.write_text("\n".join(lines), encoding="utf-8")
    b = tokens_of(preprocess(tmp, test, cl_args), tmp, dirs)
    differ = not compare(test + " (one #define removed)", a, b, {}, None, "repo", "variant")
    shutil.rmtree(tmp, ignore_errors=True)
    ok = same and differ
    print(f"TUSRC SELF-TEST: {'PASS' if ok else 'FAIL'} (same tree: {'same' if same else 'DIFFERENT'}, "
          f"one #define removed: {'different' if differ else 'SAME'})")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("tests", nargs="*", help="test files, relative to the roots (tests\\x_test.cpp)")
    ap.add_argument("--old-root", type=Path)
    ap.add_argument("--new-root", type=Path, default=ROOT)
    ap.add_argument("--cl-args")
    ap.add_argument("--rename", action="append", default=[], metavar="A=B")
    ap.add_argument("--dirs", nargs="+", default=["src", "sensors", "installer"])
    ap.add_argument("--ignore-file", action="store_true")
    ap.add_argument("--multiset", action="store_true")
    ap.add_argument("--keep", type=Path)
    ap.add_argument("--self-test", metavar="TEST")
    args = ap.parse_args()
    cl_args = args.cl_args or default_cl_args()
    if args.self_test:
        return self_test(args.self_test, cl_args, args.dirs)
    if not args.old_root or not args.tests:
        ap.error("--old-root and at least one test are needed (or --self-test)")
    renames = {}
    for r in args.rename:
        a, sep, b = r.partition("=")
        if not sep or not a or not b:
            ap.error(f"--rename {r}: A=B")
        renames[a] = b
    if args.ignore_file and args.multiset:
        ap.error("--ignore-file or --multiset, not both (--multiset already ignores the files)")
    mode = "multiset" if args.multiset else "ignore-file" if args.ignore_file else "file"
    return run(args.tests, args.old_root.resolve(), args.new_root.resolve(), cl_args, renames, args.dirs, args.keep, mode)


if __name__ == "__main__":
    sys.exit(main())
