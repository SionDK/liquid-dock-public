#!/usr/bin/env python3
"""dock.log 문구 목록: src의 Log( 호출에서 형식 문자열 리터럴을 뽑아 정렬해 찍는다.

  python tools\\refactor\\logkeys.py                    > before.txt   (기본: 고정 문구, 호출마다 한 줄, 정렬)
  python tools\\refactor\\logkeys.py --mode keys                       (줄 키: 첫 ':' 앞, 중복 없이)
  python tools\\refactor\\logkeys.py --mode full --where               (리터럴 그대로 + 파일:줄, 읽기용)
  python tools\\refactor\\logkeys.py --rev HEAD~1 > base.txt            (git 리비전의 src에서)
  python tools\\refactor\\logkeys.py --diff HEAD~1                      (그 리비전과 작업 트리의 목록 차이만)

리팩토링 전후로 목록을 diff하면 dock.log 문구가 바뀌었는지 알 수 있다. 파일 사이로 Log 줄을 옮기기만 하면 목록은 같다
(파일 이름은 기본 출력에 넣지 않는다).

하는 일:
  - src\\*.cpp, *.h(--rev면 그 리비전의 src/)를 작은 토큰 분석기로 읽는다: // · /* */ 주석, 문자열·문자 리터럴(L, u8, u, U 접두,
    R"구분(...)구분" 날 문자열 포함)을 건너뛰므로 주석 속 'Log('는 세지 않는다. 전처리 줄도 그냥 토큰으로 읽는다
    (#ifdef LD_DEV 안의 호출도 센다; release 빌드에는 없는 줄이라는 뜻).
  - 'Log' 식별자 바로 뒤가 '('이고 앞이 '.', '->', 식별자'::'가 아닌 것(전역 '::Log('는 셈)을 호출로 본다.
    첫 인자가 이어 붙인 리터럴(L"a" L"b", 여러 줄에 걸친 것)이면 그 내용을 소스에 쓰인 글자 그대로(이스케이프 포함) 잇는다.
    첫 인자가 리터럴이 아니면(정의·선언 'void Log(const wchar_t* fmt, ...)', 변수) '<비리터럴>'로 따로 센다.
  - 모드: fixed(기본) = 리터럴에서 printf 형식 지정자(%ls, %.1f, %08lx ...)를 '%'로 바꾼 고정 문구, 호출마다 한 줄.
          keys = fixed의 첫 ': ' 앞(예: 'app click', 'magnify'), 중복 없이. ': '가 없으면 첫 형식 지정자 앞(앞뒤 공백 뺌).
          full = 리터럴 그대로, 호출마다 한 줄.
  - 끝에 '# ' 로 시작하는 요약 줄(호출 수, 서로 다른 문구 수, 키 수, 비리터럴 수)을 찍는다(--no-summary로 끔).

못 보는 것:
  - 실행 때 만들어지는 문구: Log(L"%ls", buf)처럼 버퍼를 넘기는 것(문구는 '%'로만 보임), LogLines·LogMissing(tray_host_settings.cpp)이
    여러 줄을 모아 쓰는 것, LogStartSession·LogWindowsVersion이 쓰는 머리 줄, 형식 인자로 들어가는 문자열(L"hidden" 따위).
    이런 문구가 바뀌면 이 목록은 같게 나온다 -> 그 함수의 소스 diff를 따로 본다.
  - 매크로로 감싼 Log 호출, Log를 가리키는 함수 포인터. (지금 src에는 없음: 확인한 것은 이 파일의 시험 기록 참고)
"""
import argparse
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FMT = re.compile(r"%[-+ #0]*(\*|\d+)?(\.(\*|\d+))?(hh|h|ll|l|L|z|j|t|I64|I32|I)?[diouxXeEfFgGaAcCsSpn%]")


def tokens(text):
    """(kind, value, line) with kind in id, str, punct; comments and white space dropped."""
    i, n, line = 0, len(text), 1
    out = []
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r\f\v\\":
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            i = j
        elif c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            word = text[i:j]
            if j < n and text[j] in "\"'" and word in ("L", "u8", "u", "U", "R", "LR", "u8R", "uR", "UR"):
                i, line = literal(text, i, j, word, line, out)
            else:
                out.append(("id", word, line))
                i = j
        elif c in "\"'":
            i, line = literal(text, i, i, "", line, out)
        elif c.isdigit():
            j = i
            while j < n and (text[j].isalnum() or text[j] in "._'"):
                j += 1
            out.append(("num", text[i:j], line))
            i = j
        else:
            two = text[i:i + 2]
            if two in ("::", "->"):
                out.append(("punct", two, line))
                i += 2
            else:
                out.append(("punct", c, line))
                i += 1
    return out


def literal(text, start, q, prefix, line, out):
    """Reads the literal whose quote is at q; appends ('str', body, line) or ('chr', ...). Returns (next index, line)."""
    quote = text[q]
    n = len(text)
    if "R" in prefix and quote == '"':
        k = text.index("(", q)
        delim = text[q + 1:k]
        end = text.index(")" + delim + '"', k)
        body = text[k + 1:end]
        out.append(("str", body, line))
        return end + len(delim) + 2, line + body.count("\n")
    j = q + 1
    while j < n and text[j] != quote:
        if text[j] == "\\":
            j += 1
        elif text[j] == "\n":
            break
        j += 1
    out.append(("str" if quote == '"' else "chr", text[q + 1:j], line))
    return j + 1, line


def log_calls(text):
    """[(line, literal or None)] for every call of Log in the text."""
    toks = tokens(text)
    calls = []
    for k, (kind, val, line) in enumerate(toks):
        if kind != "id" or val != "Log":
            continue
        if k + 1 >= len(toks) or toks[k + 1][1] != "(":
            continue
        if k > 0:
            prev = toks[k - 1]
            if prev[1] in (".", "->"):
                continue
            if prev[1] == "::" and k > 1 and toks[k - 2][0] == "id":
                continue
        j, parts = k + 2, []
        while j < len(toks) and toks[j][0] == "str":
            parts.append(toks[j][1])
            j += 1
        if parts and j < len(toks) and toks[j][1] in (",", ")"):
            calls.append((line, "".join(parts)))
        else:
            calls.append((line, None))
    return calls


def fixed(s):
    return FMT.sub(lambda m: "%%" if m.group(0) == "%%" else "%", s)


def key(s):
    f = fixed(s)
    if ": " in f:
        return f.split(": ", 1)[0].strip()
    if f.endswith(":"):
        return f[:-1].strip()
    return f.split("%", 1)[0].strip() or f.strip()


def sources(rev):
    if rev is None:
        for p in sorted((ROOT / "src").glob("*")):
            if p.suffix in (".cpp", ".h"):
                yield p.name, p.read_bytes()
        return
    names = subprocess.run(["git", "ls-tree", "--name-only", rev, "src/"], cwd=ROOT, capture_output=True)
    if names.returncode:
        raise SystemExit(f"git ls-tree {rev} 실패: {names.stderr.decode('utf-8', 'replace').strip()}")
    for name in sorted(names.stdout.decode().split()):
        if name.endswith((".cpp", ".h")):
            data = subprocess.run(["git", "show", f"{rev}:{name}"], cwd=ROOT, capture_output=True).stdout
            yield name.split("/")[-1], data


def collect(rev, mode, where):
    rows, nonlit = [], []
    for name, data in sources(rev):
        text = data.decode("utf-8", "replace")
        if text.startswith("﻿"):
            text = text[1:]
        for line, lit in log_calls(text):
            if lit is None:
                nonlit.append(f"{name}:{line}")
                continue
            v = {"fixed": fixed, "full": lambda s: s, "keys": key}[mode](lit)
            rows.append((v, f"{name}:{line}", lit))
    return rows, nonlit


def render(rows, nonlit, mode, where, summary):
    out = []
    if mode == "keys":
        for k in sorted({r[0] for r in rows}):
            out.append(k)
    else:
        for v, loc, _ in sorted(rows):
            out.append(f"{v}\t{loc}" if where else v)
    if summary:
        out.append(f"# Log 호출 {len(rows)}개 (리터럴), 서로 다른 고정 문구 {len({fixed(r[2]) for r in rows})}개, "
                   f"키 {len({key(r[2]) for r in rows})}개, 비리터럴 {len(nonlit)}개" + (f": {', '.join(nonlit)}" if nonlit else ""))
    return out


def main():
    ap = argparse.ArgumentParser(description="src의 Log( 문구 목록 (tools/refactor/README.md)")
    ap.add_argument("--mode", choices=["fixed", "keys", "full"], default="fixed")
    ap.add_argument("--where", action="store_true", help="파일:줄을 덧붙임 (diff용이 아님)")
    ap.add_argument("--rev", help="git 리비전의 src/에서 (기본: 작업 트리)")
    ap.add_argument("--diff", metavar="REV", help="REV와 작업 트리(또는 --rev)의 목록 차이만 찍음 (다중집합)")
    ap.add_argument("--no-summary", action="store_true")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace"); sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    rows, nonlit = collect(args.rev, args.mode, args.where)
    if args.diff:
        brows, _ = collect(args.diff, args.mode, False)
        a = Counter(r[0] for r in brows)
        b = Counter(r[0] for r in rows)
        if args.mode == "keys":
            a, b = Counter(set(a)), Counter(set(b))
        gone, new = a - b, b - a
        for v, n in sorted(gone.items()):
            print(f"- {v}" + (f"  (x{n})" if n > 1 else ""))
        for v, n in sorted(new.items()):
            print(f"+ {v}" + (f"  (x{n})" if n > 1 else ""))
        print(f"# {args.diff} -> {args.rev or '작업 트리'}: 사라짐 {sum(gone.values())}, 새로 {sum(new.values())} ({args.mode})")
        return 1 if gone or new else 0
    for line in render(rows, nonlit, args.mode, args.where, not args.no_summary):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
