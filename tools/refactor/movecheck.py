#!/usr/bin/env python3
"""'옮기기만' 검사: 두 리비전 사이에서 사라진 줄 묶음이 다른 곳에 글자 그대로 나타났는지 본다.

  python tools\\refactor\\movecheck.py --from HEAD~1 --to HEAD                       (src\\ 아래 바뀐 파일 전부)
  python tools\\refactor\\movecheck.py --from HEAD                                    (--to 없음 = 작업 트리, 새 파일 포함)
  python tools\\refactor\\movecheck.py --from HEAD --files src/dock.cpp src/dock_render.cpp   (이 파일들에서 사라진 줄만 원본으로)
  python tools\\refactor\\movecheck.py --from A --to B -v                              (옮긴 묶음 목록과 짧은 조각까지)

하는 일:
  1. --paths(기본 src) 아래에서 두 리비전 사이에 바뀐 파일마다 옛 판과 새 판을 줄 단위로 diff(difflib)한다.
     줄은 바이트로 읽어 끝의 CR만 떼고 견준다(CRLF/LF가 바뀐 파일, BOM이 바뀐 파일은 따로 알린다).
  2. 사라진 줄(원본: --files를 주면 그 파일들, 아니면 바뀐 파일 전부)의 연속 묶음마다, 더해진 줄(모든 바뀐 파일,
     같은 파일 안의 다른 자리 포함) 가운데 글자 그대로 이어지는 가장 긴 묶음을 찾아 짝짓는다(빈 줄은 건너뛰고
     견준다). --min-block(기본 2)줄 이상 이어져야 '옮김'.
  3. 남은 줄 가운데 한 줄짜리는 남은 더해진 줄과 글자가 같으면 '짧은 조각'으로 짝짓는다(위치는 보장 못함, -v로 목록).
  4. 그래도 남은 줄은 '꼴만 바뀜', '접착', '주석', 그리고 나머지 '바뀐 줄'로 나눈다.
     - 꼴만 바뀜: 아래를 지운 뒤 글자가 같은 줄의 짝.
       * 앞의 static/inline - 들여쓰지 않은 줄(파일 범위)에서만. 함수·클래스 안의 static은 뜻이 바뀌므로 지우지 않는다.
       * 기본 인자 '= 값' - 함수 머리 꼴의 줄(앞에 형식 낱말이 있고, 첫 낱말이 return/if/for 같은 문장 낱말이 아닌
         'T 이름(...)' 줄)에서만, ==·<=·>=·!=·+= 들은 빼고. 그래서 'if (n >= 3)', 'for (int i = 0; ...)'는 해당 없음.
         한쪽에만 '= 값'이 있거나 양쪽 값이 같을 때만 짝짓는다('= 0'과 '= 1'은 바뀐 줄).
       * 끝의 // 주석 - 문자열·문자 리터럴 밖의 '//'부터만(L"http://..."는 그대로).
       * 공백 - 리터럴 밖에서만, 그리고 낱말 글자 둘 사이·연산자 글자 둘 사이의 공백은 하나로 남긴다('5 - -3' != '5 --3').
     - 접착: #include, #pragma once, include guard('#ifndef X', 값 없는 '#define X', '#endif'뿐: 값 있는 #define은
       바뀐 줄), namespace 여닫기, 혼자 선 괄호, 접근 지정자, using, 전방 선언, extern, 선언 한 줄('T 이름(...);' 꼴,
       첫 낱말이 return/throw/delete/else/new/case/goto/co_return 들이면 문장이라 제외). 들여쓴 선언 꼴 줄(함수 몸
       안의 'std::lock_guard<std::mutex> lock(g_mu);'와 구별할 수 없음)은 반대쪽에 같은 글자(들여쓰기 빼고)의 줄이
       있을 때만 접착이고, 없으면 바뀐 줄이다. 접착은 양쪽 사이에 글자(들여쓰기 빼고)로 짝짓고, 짝 없는 줄은 '?'로 찍는다.
  5. 짝지은 묶음이 원본과 같은 순서로 놓였는지(원본·대상 파일 쌍마다) 본다: 순서가 뒤집힌 묶음 수를 찍는다.
     이 검사는 짝지은 묶음끼리의 순서만 본다. 그래서 같은 파일 안에서 옮긴 묶음(바뀌지 않은 줄을 넘어간 것)은 따로
     '같은 파일 안에서 옮긴 묶음'으로 옛·새 줄 범위와 함께 늘 찍는다(-v 없이도).
  6. --files를 주면, 그 원본 그룹 밖에서 바뀐 파일(더해진 줄이 원본 그룹의 줄로 다 설명되지 않는 파일)을 따로 찍는다.

판정: '바뀐 줄'과 '주석' 줄이 양쪽(사라진 쪽·더해진 쪽) 모두 0이면 'MOVE-ONLY: 예'(종료 코드 0). 꼴만 바뀜, 접착(짝
없는 접착 수 따로), 순서 뒤집힘, 같은 파일 안 이동, 짧은 조각이 있으면 판정 줄 '예 (눈으로 확인할 것: ...)'에 수를 적고
모두 나열한다(사람이 읽고 허용할지 정한다). --strict면 꼴만 바뀐 줄, 짝 없는 접착, 순서 뒤집힘, 같은 파일 안 이동, 짧은
조각이 하나라도 있으면 '아니오 (--strict: ...)'다(새 파일의 include·선언·extern은 짝이 없으므로, 파일을 나누는 단계는
--strict 없이 돌리고 '?' 목록을 읽는다). 아니면 'MOVE-ONLY: 아니오'(종료 코드 1).

한계 (알고 쓸 것):
  - 줄 글자만 본다. 옮긴 묶음이 다른 namespace·다른 가짜(fake) 아래에 놓여 뜻이 바뀌는 것, 정적 초기화 순서,
    #ifdef 안팎으로 옮긴 것, 같은 이름의 다른 정의를 덮는 것은 못 본다 -> objdiff.py(기계어)와 테스트로 본다.
  - 짧은 조각(한 줄)과 '}' 같은 흔한 줄은 아무 자리에나 짝지어질 수 있다. 이웃한 두 줄 맞바꿈 같은 한 줄 이동은
    짧은 조각으로 나온다: 판정 줄의 '짧은 조각 n'을 보면 -v로 목록을 읽는다(순서 검사는 이것을 잡지 못한다).
  - '접착'·'꼴만 바뀜'은 정규식 짐작이다. 들여쓰지 않은 선언 한 줄은 짝이 없어도 접착이므로, 함수 선언의
    시그니처가 바뀐 것이 '?' 접착 두 줄(사라짐·더해짐)로 보일 수 있다(눈으로 견준다; --strict는 실패). 기본 인자 빼기는
    한 줄 안의 '= 값'만 다루고, 정의에서 뺀 기본 값과 선언에 넣은 기본 값이 같은지는 보지 않는다(선언은 '?' 접착으로 찍힘).
    한 줄 /* */ 주석은 리터럴처럼 다루지 않는다(그 안의 글자는 그대로 견줌).
  - 여러 줄에 걸친 한 문장을 다르게 줄바꿈한 것은 '바뀐 줄'로 나온다(글자 그대로가 아니므로 맞는 판정).
  - git이 필요하다(리비전은 git show로 읽음). 작업 트리 쪽은 추적 안 된 새 파일도 넣는다(.gitignore 제외).
"""
import argparse
import difflib
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEXT_EXT = {".cpp", ".h", ".hpp", ".c", ".inl", ".rc", ".bat", ".py", ".txt", ".md", ".ini", ".hlsl"}


def git(*args, check=True):
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True)
    if check and r.returncode != 0:
        raise SystemExit(f"git {' '.join(args)} 실패: {r.stderr.decode('utf-8', 'replace').strip()}")
    return r.stdout


def changed_files(a, b, paths):
    if b:
        names = git("diff", "--name-only", "--no-renames", a, b, "--", *paths).decode("utf-8").split("\n")
    else:
        names = git("diff", "--name-only", "--no-renames", a, "--", *paths).decode("utf-8").split("\n")
        names += git("ls-files", "--others", "--exclude-standard", "--", *paths).decode("utf-8").split("\n")
    return sorted({n.strip() for n in names if n.strip()})


def content(rev, path):
    """bytes or None (no such file at that revision / in the work tree)."""
    if rev is None:
        p = ROOT / path
        return p.read_bytes() if p.exists() else None
    r = subprocess.run(["git", "show", f"{rev}:{path}"], cwd=ROOT, capture_output=True)
    return r.stdout if r.returncode == 0 else None


def split_lines(data):
    if data is None:
        return [], {"crlf": 0, "lf": 0, "bom": False}
    bom = data.startswith(b"\xef\xbb\xbf")
    if bom:
        data = data[3:]
    parts = data.split(b"\n")
    if parts and parts[-1] == b"":
        parts.pop()
    crlf = sum(1 for p in parts if p.endswith(b"\r"))
    return [p[:-1] if p.endswith(b"\r") else p for p in parts], {"crlf": crlf, "lf": len(parts) - crlf, "bom": bom}


def show(b):
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        return b.decode("cp949", errors="replace")


# ------------------------------------------------------------ classification of leftovers
# A line that starts with one of these words is a statement, not a declaration (`return Compute(n, 1);`).
STATEMENT_WORDS = {b"return", b"throw", b"delete", b"else", b"co_return", b"co_yield", b"co_await", b"goto", b"new",
                   b"case", b"default", b"do", b"if", b"while", b"for", b"switch", b"sizeof", b"break", b"continue"}
GLUE = [
    ("include", re.compile(rb"^\s*#\s*(include|pragma\s+once)\b")),
    # include guard only: '#ifndef X', '#define X' with no value, '#endif' (a '#define X 16' is a changed line)
    ("guard", re.compile(rb"^\s*#\s*(ifndef\s+\w+|define\s+\w+|endif)\s*(//.*|/\*.*\*/\s*)?$")),
    ("namespace", re.compile(rb"^\s*(inline\s+)?namespace(\s+[\w:]+)?\s*\{\s*(//.*)?$")),
    ("namespace", re.compile(rb"^\s*\}\s*(;\s*)?//\s*(anonymous\s+)?namespace\b.*$")),
    ("brace", re.compile(rb"^\s*[{}]\s*;?\s*$")),
    ("access", re.compile(rb"^\s*(public|private|protected)\s*:\s*$")),
    ("using", re.compile(rb"^\s*using\s+[^;]+;\s*(//.*)?$")),
    ("forward", re.compile(rb"^\s*(struct|class|enum(\s+class)?|union)\s+\w+\s*;\s*(//.*)?$")),
    ("extern", re.compile(rb"^\s*extern\s+[^;{]*;\s*(//.*)?$")),
    # a one-line declaration: return type, name, parameters, ';' (no body)
    # (every word before the name ends in white space, so the match cannot backtrack badly)
    ("declaration", re.compile(rb"^\s*(?:[\w:<>,*&]+\s+)+[*&]*[\w:~]+\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:=\s*0\s*)?;\s*(?://.*)?$")),
]
COMMENT = re.compile(rb"^\s*(//|/\*|\*)")


def first_word(line):
    m = re.match(rb"\s*([A-Za-z_]\w*)", line)
    return m.group(1) if m else b""


def glue_kind(line):
    for kind, rx in GLUE:
        if rx.match(line):
            if kind == "declaration" and first_word(line) in STATEMENT_WORDS:
                return None
            return kind
    return None


def indented(line):
    return line[:1] in (b" ", b"\t")


def code_spans(line):
    """[(text, is_code)]: the line cut into code and string/char literal parts (so '//' or spaces inside L"..." stay)."""
    out, i, start, n = [], 0, 0, len(line)
    while i < n:
        c = line[i:i + 1]
        if c in (b'"', b"'"):
            raw = c == b'"' and i > 0 and line[i - 1:i] == b"R"
            if i > start:
                out.append((line[start:i], True))
            j = i + 1
            if raw:
                m = re.match(rb'([^()\\\s]{0,16})\(', line[j:])
                end = line.find(b")" + (m.group(1) if m else b"") + b'"', j) if m else -1
                j = n if end < 0 else end + len(m.group(1)) + 2
            else:
                while j < n and line[j:j + 1] != c:
                    j += 2 if line[j:j + 1] == b"\\" else 1
                j = min(j + 1, n)
            out.append((line[i:j], False))
            i = start = j
            continue
        if line[i:i + 2] == b"//":
            break                                   # the rest is a comment
        i += 1
    if start < min(i, n):
        out.append((line[start:i], True))
    return out


# a function head (declaration or definition) on one line: at least one type word, a name, '(' ... ')'
FUNC_HEAD = re.compile(rb"^\s*(?:[\w:<>,*&~]+\s+)+[*&]*[\w:~]+\s*\(.*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:;|\{.*)?\s*$")
# '= value' up to ',' or ')' where '=' is not part of ==, <=, >=, !=, +=, ... (a default argument)
DEFAULT_ARG = re.compile(rb"\s*(?<![=!<>+\-*/%&|^])=(?!=)\s*[^,()=]+(?=[,)])")


def is_func_head(line):
    return bool(FUNC_HEAD.match(line)) and first_word(line) not in STATEMENT_WORDS


def default_args(line):
    """The '= value' texts of a function head line (outside literals), or [] for any other line."""
    if not is_func_head(line):
        return []
    return [re.sub(rb"\s+", b"", m.group(0)) for t, code in code_spans(line) if code for m in DEFAULT_ARG.finditer(t)]


WORD_CH = re.compile(rb"\w")
OP_CH = re.compile(rb"[-+*/%&|^<>=!:.]")


def squeeze(code):
    """Spaces out: kept (as one) only between two word characters or two operator characters (a - -b, unsigned int)."""
    def keep(m):
        a, b = code[m.start() - 1:m.start()], code[m.end():m.end() + 1]
        if a and b and ((WORD_CH.match(a) and WORD_CH.match(b)) or (OP_CH.match(a) and OP_CH.match(b))):
            return b" "
        return b""
    return re.sub(rb"\s+", keep, code)


def loose(line):
    """The 'shape only' key: leading static/inline dropped only on a line that is not indented (file scope; inside
    a function or class 'static' changes the meaning), default arguments dropped only on a function head line and
    only a real '= value' (not ==, <=, >=, !=, not 'for (int i = 0; ...)'), the trailing // comment dropped (not a
    '//' inside a string), and spaces dropped outside string/char literals."""
    head = is_func_head(line)
    parts = []
    for t, code in code_spans(line):
        if code:
            if head:
                t = DEFAULT_ARG.sub(b"", t)
            t = squeeze(t)
        parts.append(t)
    s = b"".join(parts).strip()
    if not indented(line):
        s = re.sub(rb"^(?:(?:static|inline)\s+)+", b"", s)
    return s


# ------------------------------------------------------------ matching
class Side:
    """One side's changed lines: list of (file, lineno, text, run id), blank lines dropped."""
    def __init__(self):
        self.lines = []
        self.runs = defaultdict(list)       # run id -> indexes into self.lines
        self.used = []
        self.pos = []                       # position of each line inside its run

    def add_run(self, file, rid, items):
        for lineno, text in items:
            if text.strip() == b"":
                continue
            self.pos.append(len(self.runs[(file, rid)]))
            self.runs[(file, rid)].append(len(self.lines))
            self.lines.append((file, lineno, text, (file, rid)))
            self.used.append(False)


def diff_file(old, new):
    """Removed runs [(rid, [(lineno, text)])] and added runs, from difflib opcodes."""
    sm = difflib.SequenceMatcher(None, old, new, autojunk=False)
    rem, add = [], []
    for k, (tag, i1, i2, j1, j2) in enumerate(sm.get_opcodes()):
        if tag in ("delete", "replace"):
            rem.append((k, [(i + 1, old[i]) for i in range(i1, i2)]))
        if tag in ("insert", "replace"):
            add.append((k, [(j + 1, new[j]) for j in range(j1, j2)]))
    return rem, add


def match_blocks(removed: Side, added: Side, min_block):
    """Greedy: for every removed run, from its first unused line, take the longest verbatim run of unused added lines."""
    index = defaultdict(list)
    for i, (_, _, text, _) in enumerate(added.lines):
        index[text].append(i)
    blocks = []                                  # (removed idx list, added idx list)
    for rid, idxs in removed.runs.items():
        pos = 0
        while pos < len(idxs):
            if removed.used[idxs[pos]]:
                pos += 1
                continue
            text = removed.lines[idxs[pos]][2]
            best = None
            for cand in index.get(text, ()):
                if added.used[cand]:
                    continue
                arun = added.runs[added.lines[cand][3]]
                ap = added.pos[cand]
                n = 0
                while (pos + n < len(idxs) and ap + n < len(arun) and not removed.used[idxs[pos + n]]
                       and not added.used[arun[ap + n]] and removed.lines[idxs[pos + n]][2] == added.lines[arun[ap + n]][2]):
                    n += 1
                if best is None or n > best[0]:
                    best = (n, arun[ap:ap + n])
            if best and best[0] >= min_block:
                r = idxs[pos:pos + best[0]]
                for i in r:
                    removed.used[i] = True
                for i in best[1]:
                    added.used[i] = True
                blocks.append((r, best[1]))
                pos += best[0]
            else:
                pos += 1
    return blocks


def match_singles(removed: Side, added: Side):
    pool = defaultdict(list)
    for i, l in enumerate(added.lines):
        if not added.used[i]:
            pool[l[2]].append(i)
    pairs = []
    for i, l in enumerate(removed.lines):
        if removed.used[i] or glue_kind(l[2]):
            continue
        c = pool.get(l[2])
        if c:
            j = c.pop(0)
            removed.used[i] = added.used[j] = True
            pairs.append((i, j))
    return pairs


def match_loose(removed: Side, added: Side):
    pool = defaultdict(list)
    for i, l in enumerate(added.lines):
        if not added.used[i] and glue_kind(l[2]) not in ("brace", "namespace"):
            pool[loose(l[2])].append(i)
    pairs = []
    for i, l in enumerate(removed.lines):
        if removed.used[i] or glue_kind(l[2]) in ("brace", "namespace"):
            continue
        key = loose(l[2])
        c = pool.get(key)
        if not (c and key):
            continue
        # a default argument may move to (or from) one side only; '= 1' on one side and '= 2' on the other is a change
        da = default_args(l[2])
        for k, j in enumerate(c):
            db = default_args(added.lines[j][2])
            if not da or not db or da == db:
                c.pop(k)
                removed.used[i] = added.used[j] = True
                pairs.append((i, j))
                break
    return pairs


def order_inversions(blocks, removed, added):
    """Per (source file, destination file): blocks taken in source order whose destination positions go backwards."""
    per = defaultdict(list)
    for r, a in blocks:
        src, dst = removed.lines[r[0]], added.lines[a[0]]
        per[(src[0], dst[0])].append((src[1], dst[1]))
    out = {}
    for k, v in per.items():
        v.sort()
        inv = sum(1 for (s1, d1), (s2, d2) in zip(v, v[1:]) if d2 < d1)
        if inv:
            out[k] = inv
    return out


def main():
    ap = argparse.ArgumentParser(description="옮기기만 했는지 줄 묶음으로 본다 (tools/refactor/README.md)")
    ap.add_argument("--from", dest="a", required=True, help="옛 리비전")
    ap.add_argument("--to", dest="b", help="새 리비전 (없으면 작업 트리)")
    ap.add_argument("--paths", nargs="+", default=["src"], help="볼 경로 (기본 src)")
    ap.add_argument("--files", nargs="+", help="원본 파일 (사라진 줄을 이 파일들에서만 찾음). 없으면 바뀐 파일 전부")
    ap.add_argument("--min-block", type=int, default=2, help="'옮김'으로 칠 가장 짧은 묶음 (빈 줄 뺀 줄 수, 기본 2)")
    ap.add_argument("--strict", action="store_true", help="꼴만 바뀐 줄, 짝 없는 접착, 순서 뒤집힘, 같은 파일 안 이동, 짧은 조각도 실패로 친다")
    ap.add_argument("--repo", type=Path, help="다른 git 저장소에서 (기본: 이 저장소)")
    ap.add_argument("-v", "--verbose", action="store_true", help="옮긴 묶음과 짧은 조각까지 찍음")
    ap.add_argument("--limit", type=int, default=200, help="목록마다 찍을 줄 수 (기본 200)")
    args = ap.parse_args()
    if args.repo:
        global ROOT
        ROOT = args.repo.resolve()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace"); sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    files = changed_files(args.a, args.b, args.paths)
    files = [f for f in files if Path(f).suffix.lower() in TEXT_EXT]
    sources = {f.replace("\\", "/") for f in args.files} if args.files else set(files)
    removed, added = Side(), Side()
    notes = []
    for f in files:
        old, oi = split_lines(content(args.a, f))
        new, ni = split_lines(content(args.b, f))
        if (oi["crlf"] and ni["lf"]) or (oi["lf"] and ni["crlf"]) or oi["bom"] != ni["bom"]:
            if old and new:
                notes.append(f"{f}: 줄 끝/BOM 바뀜 (옛 CRLF {oi['crlf']}·LF {oi['lf']}·BOM {oi['bom']}, "
                             f"새 CRLF {ni['crlf']}·LF {ni['lf']}·BOM {ni['bom']})")
        rem, add = diff_file(old, new)
        if f in sources:
            for rid, items in rem:
                removed.add_run(f, rid, items)
        for rid, items in add:
            added.add_run(f, rid, items)
    for f in sorted(sources - set(files)):
        notes.append(f"{f}: --files에 있지만 바뀌지 않음")

    blocks = match_blocks(removed, added, args.min_block)
    singles = match_singles(removed, added)
    looses = match_loose(removed, added)
    inversions = order_inversions(blocks, removed, added)

    # Glue lines left on both sides are paired by their text (without the indentation). An unpaired glue line is
    # listed with '?' and fails --strict. A declaration-looking line that is indented (inside a function or a class:
    # 'std::lock_guard<std::mutex> lock(g_mu);' looks like a declaration) counts as glue only when it is paired.
    def glue_left(side):
        return [(i, glue_kind(l[2])) for i, l in enumerate(side.lines) if not side.used[i] and glue_kind(l[2])]
    rglue, aglue = glue_left(removed), glue_left(added)
    rtexts = Counter(removed.lines[i][2].strip() for i, _ in rglue)
    atexts = Counter(added.lines[i][2].strip() for i, _ in aglue)
    both = rtexts & atexts

    def paired_set(side, glue):
        left, out = Counter(both), set()
        for i, _ in glue:
            t = side.lines[i][2].strip()
            if left[t] > 0:
                left[t] -= 1
                out.add(i)
        return out
    rpaired, apaired = paired_set(removed, rglue), paired_set(added, aglue)

    def leftovers(side, paired):
        cats = defaultdict(list)
        for i, l in enumerate(side.lines):
            if side.used[i]:
                continue
            g = glue_kind(l[2])
            if g == "declaration" and indented(l[2]) and i not in paired:
                g = None                                # a statement inside a body, not a declaration
            if g:
                cats[f"glue:{g}"].append(l + (i in paired,))
            else:
                cats["comment" if COMMENT.match(l[2]) else "changed"].append(l + (None,))
        return cats
    rcat, acat = leftovers(removed, rpaired), leftovers(added, apaired)
    same_file = [(r, a) for r, a in blocks if removed.lines[r[0]][0] == added.lines[a[0]][0]]

    moved_lines = sum(len(r) for r, _ in blocks)
    print(f"movecheck {args.a} -> {args.b or '작업 트리'} (경로 {' '.join(args.paths)}; 원본 "
          f"{'바뀐 파일 전부' if not args.files else ', '.join(sorted(sources))})")
    print(f"바뀐 파일 {len(files)}개. 사라진 줄 {len(removed.lines)}, 더해진 줄 {len(added.lines)} (빈 줄 뺌)")
    print(f"옮긴 묶음 {len(blocks)}개 ({moved_lines}줄, --min-block {args.min_block}), 짧은 조각 {len(singles)}줄, "
          f"꼴만 바뀜 {len(looses)}줄")
    for n in notes:
        print("  알림:", n)

    per_pair = defaultdict(int)
    for r, a in blocks:
        per_pair[(removed.lines[r[0]][0], added.lines[a[0]][0])] += len(r)
    print("\n옮긴 곳 (원본 -> 대상: 줄 수)")
    for (s, d), n in sorted(per_pair.items()):
        flag = f"  [순서 뒤집힘 {inversions[(s, d)]}곳]" if (s, d) in inversions else ""
        print(f"  {s} -> {d}: {n}{' (같은 파일 안)' if s == d else ''}{flag}")

    def dump(title, items, sign):
        if not items:
            return
        unpaired = sum(1 for it in items if it[4] is False)
        print(f"\n{title} ({len(items)}" + (f", 짝 없음 {unpaired}: '?' 줄" if unpaired else "") + ")")
        for f, ln, text, _, paired in items[: args.limit]:
            mark = "?" if paired is False else " " if paired else ""
            print(f"  {sign}{mark} {f}:{ln}: {show(text)}")
        if len(items) > args.limit:
            print(f"  ... {len(items) - args.limit}줄 더")

    if same_file:
        print(f"\n같은 파일 안에서 옮긴 묶음 ({len(same_file)}: 문장 순서가 바뀌었을 수 있음 - 눈으로 볼 것)")
        for r, a in same_file[: args.limit]:
            s, d = removed.lines[r[0]], added.lines[a[0]]
            print(f"  {s[0]}: 옛 {s[1]}-{removed.lines[r[-1]][1]}줄 -> 새 {d[1]}-{added.lines[a[-1]][1]}줄 ({len(r)}줄): "
                  f"{show(s[2]).strip()[:80]}")
    if args.verbose:
        print("\n옮긴 묶음")
        for r, a in blocks:
            s, d = removed.lines[r[0]], added.lines[a[0]]
            print(f"  {s[0]}:{s[1]}-{removed.lines[r[-1]][1]} -> {d[0]}:{d[1]}-{added.lines[a[-1]][1]} ({len(r)}줄)")
        print(f"\n짧은 조각 ({len(singles)})")
        for i, j in singles[: args.limit]:
            s, d = removed.lines[i], added.lines[j]
            print(f"  {s[0]}:{s[1]} -> {d[0]}:{d[1]}: {show(s[2])}")
    if looses:
        print(f"\n꼴만 바뀐 줄 ({len(looses)}: static/inline·기본 인자·끝 주석·공백)")
        for i, j in looses[: args.limit]:
            s, d = removed.lines[i], added.lines[j]
            print(f"  - {s[0]}:{s[1]}: {show(s[2])}")
            print(f"  + {d[0]}:{d[1]}: {show(d[2])}")
    for cat in sorted(set(rcat) | set(acat)):
        if cat.startswith("glue:"):
            dump(f"접착 {cat[5:]} - 사라짐", rcat.get(cat, []), "-")
            dump(f"접착 {cat[5:]} - 더해짐", acat.get(cat, []), "+")
    dump("주석 - 사라짐", rcat.get("comment", []), "-")
    dump("주석 - 더해짐", acat.get("comment", []), "+")
    dump("바뀐 줄 - 사라짐 (어디에도 글자 그대로 없음)", rcat.get("changed", []), "-")
    dump("바뀐 줄 - 더해짐 (어느 원본에서도 오지 않음)", acat.get("changed", []), "+")

    if args.files:
        outside = sorted({l[0] for i, l in enumerate(added.lines) if not added.used[i] and l[0] not in sources
                          and not glue_kind(l[2])})
        touched = sorted(set(files) - sources)
        if touched:
            print("\n원본 그룹 밖에서 바뀐 파일: " + ", ".join(touched))
            if outside:
                print("  그 가운데 원본에서 오지 않은 줄이 있는 파일: " + ", ".join(outside))

    nchg = {k: len(rcat.get(k, [])) + len(acat.get(k, [])) for k in ("changed", "comment")}
    glue_items = [it for k, v in list(rcat.items()) + list(acat.items()) if k.startswith("glue:") for it in v]
    nglue = len(glue_items)
    nglue_unpaired = sum(1 for it in glue_items if it[4] is False)
    ninv = sum(inversions.values())
    nsame = len(same_file)
    ok = nchg["changed"] == 0 and nchg["comment"] == 0
    strict_bad = [k for k, v in (("꼴만 바뀐 줄", len(looses)), ("짝 없는 접착", nglue_unpaired), ("순서 뒤집힘", ninv),
                                 ("같은 파일 안 이동", nsame), ("짧은 조각", len(singles))) if v]
    if args.strict and strict_bad:
        ok = False
    print(f"\n요약: 바뀐 줄 -{len(rcat.get('changed', []))}/+{len(acat.get('changed', []))}, 주석 "
          f"-{len(rcat.get('comment', []))}/+{len(acat.get('comment', []))}, 접착 {nglue}(짝 없음 {nglue_unpaired}), "
          f"꼴만 바뀜 {len(looses)}, 순서 뒤집힘 {ninv}, 같은 파일 안 이동 {nsame}묶음, 짧은 조각 {len(singles)}")
    check = ", ".join(f"{k} {v}" for k, v in (("꼴만 바뀐 줄", len(looses)), ("접착", nglue), ("짝 없는 접착", nglue_unpaired),
                                              ("순서 뒤집힘", ninv), ("같은 파일 안 이동", f"{nsame}묶음" if nsame else 0),
                                              ("짧은 조각", len(singles))) if v)
    if ok:
        print("MOVE-ONLY: " + (f"예 (눈으로 확인할 것: {check})" if check else "예"))
    elif args.strict and nchg["changed"] == 0 and nchg["comment"] == 0:
        print("MOVE-ONLY: 아니오 (--strict: " + ", ".join(strict_bad) + ")")
    else:
        print("MOVE-ONLY: 아니오")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
