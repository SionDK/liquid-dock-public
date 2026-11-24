#!/usr/bin/env python3
"""리팩토링 한 단계의 관문: build.bat -> run_regressions --logged -> run_dock_interactions 를 차례로 돌리고 기준과 대조한다.

  python tools\\refactor\\gate.py --baseline userResult\\<기준 폴더> --out build\\refactor-scratch\\gate-1
  python tools\\refactor\\gate.py --baseline <기준> --out <폴더> --dev-compile         (LD_DEV 블록도 경고 0인지)
  python tools\\refactor\\gate.py --baseline <기준> --from-logs <폴더>                  (돌리지 않고 이미 있는 로그만 대조)
  python tools\\refactor\\gate.py --record --out <새 기준 폴더>                          (다른 PC에서 기준을 새로 잡기)

보는 것 (모두 바이트로 읽고 errors='replace', 한글 CP949 로그가 섞여도 죽지 않는다):
  a. build.bat: 종료 코드 0, 'warning'/'error' 낱말 0, 'is in use - skipped' 줄 없음, 도우미(sensors_main.cpp)와
     설치 파일(setup_main.cpp)을 실제로 컴파일했는지(cl이 찍는 파일 이름 줄). 빌드가 끝난 *뒤에* 세 exe의 sha256을
     재서 기준(hash-after-build.txt, 'sha256 *경로' 꼴)과 견준다. 도우미가 다르면 FAIL(--allow-helper-change면 경고).
     LiquidDock.exe·Setup이 다른 것은 정보(--require-same-exe면 FAIL). build\\*.obj 가운데 src\\*.cpp에 짝이 없는 옛 .obj
     (목록을 안 고친 bat이 옛 코드로 링크할 수 있음)는 FAIL.
  b. run_regressions.bat --logged: 테스트별 결과 줄을 뽑아 기준(regressions-results.txt)과 검사 수를 견준다.
     결과 줄 꼴 여섯 가지(아래 LEGACY)와 '이름: ALL PASSED (N checks, F failures)'를 모두 알아보고, 한 테스트에
     옛 꼴과 새 꼴이 함께 있어도 꼴마다 따로 센다. 기준에 있던 꼴이 사라지거나 수가 줄면 FAIL, 늘면 정보
     (--exact-counts면 FAIL), 새 꼴·새 테스트는 정보. bat의 for 목록에 있는데 결과 줄이 없는 테스트도 FAIL
     (rr-tests-1: 충돌한 테스트는 끝 줄만으로는 안 보인다). '^PASS' 줄을 다중집합으로 견줘 사라진 줄은 FAIL,
     새 줄은 정보. 견주기 전에 양쪽 줄에서 그날·그 PC 값을 자리표로 바꾼다(아래 PASS_VOLATILE: start_menu_run_test의
     판정 문자열 '"works|failed <Windows 빌드> <1601년부터의 UTC 날짜 수>"'. 날짜 수는 UTC 자정마다, 빌드는 Windows
     업데이트마다 바뀌어 기준을 잡은 날과 다른 날에는 늘 '사라짐'이 된다). 테스트 자신은 그 값을 그대로 검사한다. '^FAIL', 'FAILED:', 도구 경고·오류 코드('warning [A-Z]+N'·'error [A-Z]+N': cl C4xxx, cl 명령줄
     D9xxx, LNK, RC ...), 'fatal error' 줄 수, 끝 줄 'all regression tests passed', 종료 코드.
  c. run_dock_interactions.bat: RESULT 줄의 cases·checks·failed_checks를 기준(interactions.log의 RESULT)과 견준다.
     컴파일 경고·오류는 b와 같은 정규식으로 센다.
  d. --dev-compile: src\\main.cpp만 build.bat의 CF + /DLD_DEV /c 로 build\\refactor-scratch\\dev\\ 에 컴파일(build\\*.obj는
     안 덮음). release 빌드는 #ifdef LD_DEV 블록을 컴파일하지 않으므로 따로 본다. 경고·오류 0이어야 PASS.
  e. tools\\llm-tasks\\run_tasks.py --self-check (읽기만: 문맥 파일, 변이 위치, check_log_guide.py --list, git diff).
     네트워크·LLM 없음. 결과는 한 줄로 찍고 기준(selfcheck.log)이 있으면 문제 수를 견준다. GATE 판정에는 넣지 않는다.
  f. HASH 줄(0-3의 특성 테스트): regressions.log와 interactions.log에서 줄 맨 앞이 'HASH: <이름> <16진수>'인 줄을 뽑아
     기준 폴더의 hash-lines.txt(같은 꼴의 줄)와 이름별로 견준다. 값이 다름 = FAIL(이름과 두 값을 찍음), 기준에 있는데
     이번에 없음 = FAIL, 새 이름 = INFO, 한 실행에서 같은 이름이 두 번 = FAIL. 'UNSTABLE: <이름> (...)' 줄(dock_render_device_test가
     세 번 그린 프레임이 같지 않아 해시를 내지 않은 장면)이 어느 한쪽에라도 있으면 그 이름은 양쪽에서 빼고 INFO로 찍는다.
     --allow-hash-change <접두어>(여러 번)는 그 접두어로 시작하는 이름의 다름을 WARN으로 낮춘다(그래서 끝 줄은 'PASS (부분)').
     기준 폴더에 hash-lines.txt가 없으면(0-3 전의 기준) 대조는 INFO '기준 없음'이고, 끝 줄은 다른 구역대로 정한다.
     --record는 hash-lines.txt를 쓴다. 실제 실행은 run_regressions 전에 build\\tests 아래의 옛 trace-*.txt(dock_layout_test가
     exe 옆에 쓰는 프레임 흔적 원문)를 지우고, 끝난 뒤 --out\\traces\\로 복사한다(--record면 기준 폴더에도 남아, dock_trace
     HASH가 다를 때 찍는 diff 명령으로 두 원문을 견줄 수 있다).

시각에 기대는 검사가 흔들렸을 때: --rerun-failed 를 주면 결과 줄이 없거나 'FAILED:'로 멈춘 테스트부터
끝까지의 테스트 exe를 bat 순서대로 한 번 더 직접 돌린다(다시 컴파일하지 않음). exe마다 따로 로그를 받아, 종료 코드 0이고
결과 줄이 하나라도 있고 실패 수가 모두 0이면 통과로 본다(이름으로 거르지 않음: 그 로그의 결과 줄은 모두 그 테스트의 것이고,
이름 없는 'ALL PASSED (0 failures)'도 그 테스트에 붙인다). 다시 돌려 모두 통과하면 '흔들림' 경고로 보고하지만, 첫 실행의
실패는 GATE에 그대로 남는다(흔들림인지는 사람이 판단). --from-logs에서는 무엇을 다시 돌릴지만 찍는다.

기준 파일: --record가 아니면, 건너뛰지 않는 구역의 기준이 모두 있어야 한다(build: hash-after-build.txt의 세 exe,
regressions: regressions-results.txt의 결과 줄과 regressions-pass-lines.txt, interactions: interactions.log의 RESULT 줄).
하나라도 없으면 아무것도 돌리지 않고 인자 오류(종료 코드 2)로 끝난다(빈 폴더·한 단계 위 폴더·앞선 --out 폴더를 기준으로
잘못 준 것). '기준 없음' 정보와 PASS 줄 SKIP은 --record에서만 나온다. --out이 기준 폴더와 같으면 인자 오류(기준 폴더
userResult\\는 git에 없어 덮으면 되살릴 수 없다). --record는 --out에 기준 파일(또는 실제로 돌릴 때 로그)이 이미 있으면
--force 없이는 거부한다.

로그 꼴: 이 도구가 돌린 로그 끝에는 'exit=N' 줄을 붙인다(기준 로그와 같은 꼴). --from-logs는 그 줄로 종료 코드를 읽는다.
실제 실행은 build 뒤 --out\\hash-after-build.txt도 쓴다(그래서 그 폴더를 --from-logs로 다시 볼 때 도우미 해시도 견준다).
regressions나 interactions를 돌린 실제 실행은 끝에(테스트 exe 해시를 견준 뒤) --out\\test-exe-hashes.txt에 그 실행의 테스트
exe 해시를 기준과 같은 꼴로 쓴다: 다음 단계가 --test-exe-hashes <앞 단계의 --out>\\test-exe-hashes.txt로 주면 그 단계에서
달라진 테스트 exe만 보인다. 판정에는 넣지 않는다.
--from-logs 폴더에 hash-after-build.txt가 없으면 FAIL(--allow-no-hash면 SKIP).
bat은 한 번에 하나씩 차례로 부른다(같은 build\\ 폴더, 고정 이름). build.bat이 실패하면 뒤는 돌리지 않는다.
끝 줄: 판정하는 구역(build, regressions, interactions, dev-compile)을 모두 검사하고 FAIL이 없으면 'GATE: PASS'. FAIL은 없지만
SKIP(건너뛴 구역, --from-logs의 옛 .obj, --allow-no-hash)이나 WARN(exit= 줄 없음, --allow-helper-change)이 있으면
'GATE: PASS (부분: 구역/항목 SKIP; ...)'.
종료 코드: 0 = GATE: PASS(또는 --record 성공), 1 = GATE: FAIL, 2 = 인자 잘못(기준 파일 없음 포함), 3 = GATE: PASS (부분: ...).
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXES = ["build/LiquidDock.exe", "build/LiquidDockSensors.exe", "build/LiquidDock-Setup.exe"]
HELPER = "build/LiquidDockSensors.exe"
SCRATCH = ROOT / "build" / "refactor-scratch"

# Result lines that carry no 'name: ALL PASSED (N checks, ...)' (in the baseline of mod.35). Each maps to its test
# (the bat's name without _test) and the meaning of its number. The unnamed 'ALL PASSED (F failures)' is printed by
# two tests; its k-th occurrence belongs to the k-th name of UNNAMED_OWNERS (their order in the bat's loop).
LEGACY = [
    ("hwinfo-line", re.compile(rb"^all hwinfo tests passed$"), "hwinfo"),
    ("recovery-line", re.compile(rb"^(\d+) recovery regression cases passed;"), "taskbar_recovery"),
    ("installer-line", re.compile(rb"^Installer cleanup regression passed;"), "installer_cleanup"),
    ("preview-line", re.compile(rb"^PASS: (\d+) preview checks, \d+ thumbnail updates$"), "window_preview"),
]
UNNAMED = re.compile(rb"^ALL PASSED \((\d+) failures\)$")
UNNAMED_OWNERS = ["tray_host", "fluent_flyout"]
NAMED = re.compile(rb"^([A-Za-z0-9_\-\\/.]+): ALL PASSED \((\d+) checks, (\d+) failures\)$")
FINAL = b"all regression tests passed"
RESULT_RE = re.compile(rb"RESULT cases=(\d+) checks=(\d+) failed_checks=(\d+)")
EXIT_RE = re.compile(rb"^exit=(-?\d+)$")
# a tool's coded diagnostic: cl C4xxx, cl command line D9xxx, link LNK4xxx, rc RC4xxx, cvtres CVT1xxx ...
TOOL_WARNING = re.compile(rb"\bwarning [A-Z]+\d+")
TOOL_ERROR = re.compile(rb"\berror [A-Z]+\d+")
# Values in '^PASS' lines that change with the day or the PC, not with the code. Both sides are masked before the
# multiset comparison. start_menu_run_test prints its verdict string "works|failed <WindowsBuild()> <Today()>"
# (src/start_menu.cpp: Today() = days since 1601 UTC, so it changes at 09:00 KST; the build changes with a Windows update).
PASS_VOLATILE = [
    (re.compile(rb"\b(works|failed) \d+\.\d+ -?\d+\b"), rb"\1 <build> <day>"),
]
# The characterization tests' hash lines (0-3): 'HASH: <name> <hex>' at the start of a line, one per name, and
# 'UNSTABLE: <name> (...)' for a scene a test could not hash this run (dock_render_device_test: three draws not alike).
HASH_RE = re.compile(rb"^HASH: (\S+) ([0-9A-Fa-f]+)\s*$")
UNSTABLE_RE = re.compile(rb"^UNSTABLE: (\S+)")
HASH_FILE = "hash-lines.txt"
TRACE_GLOB = "trace-*.txt"          # the frame traces dock_layout_test leaves next to its exe (build\tests\dock-layout)
BASELINE_FILES = ("hash-after-build.txt", "regressions-results.txt", "regressions-pass-lines.txt", "test-exe-hashes.txt",
                  HASH_FILE)
LOG_FILES = ("build.log", "regressions.log", "interactions.log")
JUDGED = ("build", "regressions", "interactions", "hashes", "dev-compile")   # sections whose SKIP/WARN make a PASS partial


# ---------------------------------------------------------------- small helpers
def lines_of(data: bytes):
    out = data.split(b"\n")
    if out and out[-1] == b"":
        out.pop()
    return [l[:-1] if l.endswith(b"\r") else l for l in out]


def read_lines(path: Path):
    return lines_of(path.read_bytes()) if path.exists() else None


def show(b: bytes) -> str:
    for enc in ("utf-8", "cp949"):
        try:
            return b.decode(enc)
        except UnicodeDecodeError:
            pass
    return b.decode("utf-8", errors="replace")


def sha256(path: Path):
    if not path.exists():
        return None
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_hash_file(path: Path):
    """{'build/X.exe': sha256} from 'sha256 *path' (sha256sum) lines; backslashes become slashes."""
    out = {}
    for l in read_lines(path) or []:
        m = re.match(rb"^([0-9a-fA-F]{64}) [ *](.+)$", l.strip())
        if m:
            out[show(m.group(2)).replace("\\", "/").lstrip("./")] = m.group(1).decode().lower()
    return out


def mask_pass(l: bytes) -> bytes:
    """A '^PASS' line with the values that change with the day or the PC replaced by placeholders (PASS_VOLATILE)."""
    for rx, sub in PASS_VOLATILE:
        l = rx.sub(sub, l)
    return l


def exit_code_in(lines):
    for l in reversed(lines or []):
        m = EXIT_RE.match(l.strip())
        if m:
            return int(m.group(1))
    return None


def bat_test_order():
    """[(id, exe relative path)] from the for loops of tests\\run_regressions.bat, in the order they run."""
    out = []
    bat = (ROOT / "tests" / "run_regressions.bat").read_bytes().decode("utf-8", "replace")
    for m in re.finditer(r"^\s*for %%T in \(([^)]*)\) do \(", bat, re.M):
        for item in m.group(1).split():
            out.append((item.replace("\\", "/").split("/")[-1], f"build/tests/{item.replace(chr(92), '/')}_test.exe"))
    return out


def norm_name(n: str) -> str:
    n = n.replace("\\", "/").split("/")[-1]
    return n[:-5] if n.endswith("_test") else n


class Report:
    def __init__(self):
        self.rows = []          # (section, item, status, detail)

    def add(self, section, item, status, detail=""):
        self.rows.append((section, item, status, detail))

    def fails(self):
        return [r for r in self.rows if r[2] == "FAIL"]


def banner(text):
    bold = sys.stdout.isatty()
    line = "!" * 78
    msg = f"{line}\n!!! {text}\n{line}"
    print(f"\033[1m{msg}\033[0m" if bold else msg)


def run_logged(cmd, log: Path, timeout, label):
    """Runs cmd (list) in ROOT with stdout+stderr to log, appends 'exit=N'. Returns (code, seconds)."""
    log.parent.mkdir(parents=True, exist_ok=True)
    print(f"[gate] {label}: {' '.join(cmd)}  ->  {log}", flush=True)
    t0 = time.time()
    with open(log, "wb") as f:
        try:
            code = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            code = -999
            f.write(b"\r\n[gate] timeout\r\n")
    if code > 0x7FFFFFFF:
        code -= 1 << 32           # NTSTATUS crash codes (0xC0000005) as the negative numbers cmd shows
    with open(log, "ab") as f:
        f.write(f"exit={code}\r\n".encode())
    secs = time.time() - t0
    print(f"[gate] {label}: exit={code} ({secs:.0f} s)", flush=True)
    return code, secs


# ---------------------------------------------------------------- a. build
def check_build(rep: Report, lines, code, hashes, base_hashes, args):
    S = "build"
    if lines is None:
        rep.add(S, "build.log", "FAIL", "로그 없음")
        return
    if code is None:
        rep.add(S, "종료 코드", "WARN", "로그에 exit= 줄이 없음")
    else:
        rep.add(S, "종료 코드", "PASS" if code == 0 else "FAIL", str(code))
    warn = [l for l in lines if re.search(rb"(?i)\bwarning\b", l)]
    err = [l for l in lines if re.search(rb"(?i)\berror\b", l)]
    rep.add(S, "warning 줄", "PASS" if not warn else "FAIL", f"{len(warn)}" + (": " + show(warn[0])[:120] if warn else ""))
    rep.add(S, "error 줄", "PASS" if not err else "FAIL", f"{len(err)}" + (": " + show(err[0])[:120] if err else ""))
    skipped = [l for l in lines if b"is in use - skipped" in l]
    rep.add(S, "'is in use - skipped'", "PASS" if not skipped else "FAIL",
            "없음" if not skipped else "도우미와 설치 파일을 다시 빌드하지 않음 -> 해시 비교가 옛 파일을 본다")
    stripped = {l.strip() for l in lines}
    for src in (b"sensors_main.cpp", b"setup_main.cpp"):
        rep.add(S, f"{src.decode()} 컴파일", "PASS" if src in stripped else "FAIL",
                "cl이 이름을 찍음" if src in stripped else "출력에 없음 (빌드가 그 앞에서 멈췄거나 건너뜀)")
    if hashes is None:
        rep.add(S, "exe 해시", "SKIP" if (args.allow_no_hash or args.record) else "FAIL",
                "이번 해시 없음 (--from-logs 폴더에 hash-after-build.txt 없음"
                + (": --allow-no-hash)" if args.allow_no_hash else "; 도우미 해시를 못 봄 - 허용하려면 --allow-no-hash)"))
        return
    for exe in EXES:
        cur, base = hashes.get(exe), base_hashes.get(exe) if base_hashes else None
        if base is None:
            rep.add(S, f"해시 {exe}", "INFO" if args.record else "FAIL", f"기준 없음, 이번 {cur}")
            continue
        if cur is None:
            rep.add(S, f"해시 {exe}", "FAIL", "파일 없음")
            continue
        same = cur == base
        if same:
            rep.add(S, f"해시 {exe}", "PASS", "기준과 같음")
        elif exe == HELPER:
            banner("관리자 도우미(LiquidDockSensors.exe)의 바이트가 기준과 다름 - 도우미 코드·리소스·버전이 바뀜")
            rep.add(S, f"해시 {exe}", "WARN" if args.allow_helper_change else "FAIL",
                    f"다름 (기준 {base[:12]}.., 이번 {cur[:12]}..)")
        else:
            rep.add(S, f"해시 {exe}", "FAIL" if args.require_same_exe else "INFO",
                    f"다름 (기준 {base[:12]}.., 이번 {cur[:12]}..)")


def check_stale_obj(rep: Report):
    cpps = {p.stem.lower() for p in (ROOT / "src").glob("*.cpp")}
    objs = sorted((ROOT / "build").glob("*.obj"))
    stale = [p.name for p in objs if p.stem.lower() not in cpps]
    rep.add("build", "옛 build\\*.obj", "PASS" if not stale else "FAIL",
            f"{len(objs)}개 모두 src\\*.cpp에 짝 있음" if not stale else
            "짝 없는 .obj: " + ", ".join(stale) + " (지우고 다시 빌드: 목록 안 고친 bat이 옛 코드로 링크함)")


# ---------------------------------------------------------------- b. regressions
def extract_results(lines, owner_of_unnamed=None):
    """[(line index, test id, kind, count, failures, raw line)] for every result line, in log order.
    owner_of_unnamed: the test whose own log this is (a rerun of one exe); its unnamed lines are given to it instead of
    to UNNAMED_OWNERS by their order in the whole regression log."""
    out, unnamed_seen = [], 0
    for i, l in enumerate(lines):
        s = l.strip()
        m = NAMED.match(s)
        if m:
            out.append((i, norm_name(show(m.group(1))), "named", int(m.group(2)), int(m.group(3)), s))
            continue
        m = UNNAMED.match(s)
        if m:
            if owner_of_unnamed:
                owner = owner_of_unnamed
            else:
                owner = UNNAMED_OWNERS[unnamed_seen] if unnamed_seen < len(UNNAMED_OWNERS) else f"?unnamed#{unnamed_seen + 1}"
            unnamed_seen += 1
            out.append((i, owner, "unnamed-line", None, int(m.group(1)), s))
            continue
        for kind, rx, owner in LEGACY:
            m = rx.match(s)
            if m:
                cnt = int(m.group(1)) if m.groups() else None
                out.append((i, owner, kind, cnt, 0, s))
                break
    return out


def by_test(results):
    d = {}
    for _, test, kind, cnt, fails, raw in results:
        d.setdefault(test, {}).setdefault(kind, []).append((cnt, fails, raw))
    return d


def fmt_kinds(kinds):
    parts = []
    for kind, vals in sorted(kinds.items()):
        for cnt, _, _ in vals:
            parts.append(f"{kind}={'-' if cnt is None else cnt}")
    return ", ".join(parts)


def check_regressions(rep: Report, lines, code, base_results_lines, base_pass_lines, args):
    S = "regressions"
    if lines is None:
        rep.add(S, "regressions.log", "FAIL", "로그 없음")
        return None
    cur = by_test(extract_results(lines))
    base = by_test(extract_results(base_results_lines or []))
    order = [t for t, _ in bat_test_order()]
    names = list(dict.fromkeys(order + list(base) + list(cur)))
    table = []
    for t in names:
        b, c = base.get(t), cur.get(t)
        if not c:
            st, why = "FAIL", "결과 줄 없음" + (" (충돌·멈춤일 수 있음: rr-tests-1)" if t in order else " (bat 목록에서도 빠짐)")
        elif not b:
            st, why = "INFO", "새 테스트"
        else:
            st, why = "PASS", "같음"
            notes = []
            for kind, bvals in b.items():
                cvals = c.get(kind)
                if not cvals:
                    st, notes = "FAIL", notes + [f"{kind} 줄이 사라짐"]
                    continue
                bc = sorted((v[0] for v in bvals), key=lambda x: -1 if x is None else x)
                cc = sorted((v[0] for v in cvals), key=lambda x: -1 if x is None else x)
                if len(cc) < len(bc):
                    st, notes = "FAIL", notes + [f"{kind} 줄 수 {len(bc)} -> {len(cc)}"]
                for x, y in zip(bc, cc):
                    if x is None or y is None or x == y:
                        continue
                    if y < x:
                        st, notes = "FAIL", notes + [f"{kind} 검사 수 줄어듦 {x} -> {y}"]
                    else:
                        if st != "FAIL":
                            st = "FAIL" if args.exact_counts else "INFO"
                        notes.append(f"{kind} 검사 수 늘어남 {x} -> {y}")
            for kind in c:
                if kind not in b:
                    if st == "PASS":
                        st = "INFO"
                    notes.append(f"결과 줄 더해짐: {fmt_kinds({kind: c[kind]})}")
            why = "; ".join(notes) if notes else why
        bad = [v for vals in (c or {}).values() for v in vals if v[1]]
        if bad:
            st, why = "FAIL", f"실패 수가 0이 아님: {show(bad[0][2])}"
        table.append((t, fmt_kinds(b) if b else "-", fmt_kinds(c) if c else "-", st, why))
        rep.add(S, f"결과 {t}", st, why)
    rep.add(S, "테스트 수", "INFO", f"이번 {len(cur)}개, 기준 {len(base)}개, bat 목록 {len(order)}개")

    # PASS lines as a multiset, with the day/PC values masked on both sides (PASS_VOLATILE)
    cur_pass = Counter(mask_pass(l) for l in lines if l.startswith(b"PASS"))
    if base_pass_lines is not None:
        bp = Counter(mask_pass(l) for l in base_pass_lines)
        gone, new = bp - cur_pass, cur_pass - bp
        masked = sum(1 for l in base_pass_lines if mask_pass(l) != l)
        rep.add(S, "^PASS 줄", "PASS" if not gone else "FAIL",
                f"이번 {sum(cur_pass.values())}, 기준 {sum(bp.values())}, 사라짐 {sum(gone.values())}, 새 줄 {sum(new.values())}"
                f" (기준의 {masked}줄은 날짜·빌드를 자리표로 바꿔 견줌)")
        for l, n in sorted(gone.items())[: args.list_limit]:
            rep.add(S, "  사라진 PASS 줄", "FAIL", (f"x{n} " if n > 1 else "") + show(l)[:200])
        for l, n in sorted(new.items())[: args.list_limit]:
            rep.add(S, "  새 PASS 줄", "INFO", (f"x{n} " if n > 1 else "") + show(l)[:200])
        if len(gone) > args.list_limit or len(new) > args.list_limit:
            rep.add(S, "  (줄 목록)", "INFO", f"앞의 {args.list_limit}개만 찍음 (--list-limit)")
    else:
        rep.add(S, "^PASS 줄", "SKIP" if args.record else "FAIL",
                f"기준 regressions-pass-lines.txt 없음 (이번 {sum(cur_pass.values())}줄)")

    def count(rx):
        return [l for l in lines if re.search(rx, l)]
    for label, rx in (("^FAIL 줄", rb"^FAIL"), ("'FAILED:' 줄", rb"FAILED:"), ("warning 코드(C/D/LNK/RC ...)", TOOL_WARNING),
                      ("error 코드(C/D/LNK/RC ...)", TOOL_ERROR), ("fatal error", rb"(?i)fatal error")):
        hit = count(rx)
        rep.add(S, label, "PASS" if not hit else "FAIL", f"{len(hit)}" + (": " + show(hit[0].strip())[:160] if hit else ""))
    has_final = any(l.strip() == FINAL for l in lines)
    rep.add(S, "끝 줄 'all regression tests passed'", "PASS" if has_final else "FAIL", "있음" if has_final else "없음")
    if code is None:
        rep.add(S, "종료 코드", "WARN", "로그에 exit= 줄이 없음")
    else:
        rep.add(S, "종료 코드", "PASS" if code == 0 else "FAIL", str(code))
    return table, cur


def rerun_candidates(lines, cur):
    """Tests to run again: from the one 'FAILED:' names (or the first without a result line) to the end of the bat order."""
    order = bat_test_order()
    failed = [norm_name(show(m.group(1))) for l in lines for m in [re.search(rb"FAILED: (\S+)", l)] if m]
    start = None
    for i, (t, _) in enumerate(order):
        if t in failed or t not in cur:
            start = i
            break
    return [] if start is None else order[start:]


def rerun_verdict(lines, code, test):
    """(passed, why) for the log of one test exe run on its own: exit 0, at least one result line, no failures."""
    res = extract_results(lines, owner_of_unnamed=test)
    if code != 0:
        return False, f"exit={code}"
    if not res:
        return False, "결과 줄 없음"
    if any(r[4] for r in res):
        return False, "실패 수가 0이 아님"
    return True, ""


def rerun(rep: Report, cands, out: Path, live: bool):
    S = "regressions"
    if not cands:
        return
    if not live:
        rep.add(S, "다시 돌릴 테스트(--rerun-failed)", "INFO", ", ".join(t for t, _ in cands) + " (--from-logs: 돌리지 않음)")
        return
    passed, failed = [], []
    for t, exe in cands:
        log = out / f"rerun-{t}.log"
        if not (ROOT / exe).exists():
            failed.append(f"{t}(exe 없음)")
            continue
        code, _ = run_logged([str(ROOT / exe)], log, 900, f"rerun {t}")
        # this log holds one test only: any result line in it is that test's (no filtering by name: the unnamed
        # 'ALL PASSED (0 failures)' of fluent_flyout would otherwise be given to tray_host and dropped)
        passed_now, why = rerun_verdict(read_lines(log) or [], code, t)
        (passed if passed_now else failed).append(t if passed_now else f"{t}({why})")
    if not failed:
        rep.add(S, "다시 돌림", "WARN", "흔들림: 다시 돌리니 모두 통과 (" + ", ".join(passed) + ") - 첫 실행의 FAIL은 그대로 둠")
    else:
        rep.add(S, "다시 돌림", "FAIL", "다시 돌려도 실패: " + ", ".join(failed))


# ---------------------------------------------------------------- c. interactions
def result_of(lines):
    for l in reversed(lines or []):
        m = RESULT_RE.search(l)
        if m:
            return tuple(int(x) for x in m.groups())
    return None


def check_interactions(rep: Report, lines, code, base_lines, args):
    S = "interactions"
    if lines is None:
        rep.add(S, "interactions.log", "FAIL", "로그 없음")
        return
    cur, base = result_of(lines), result_of(base_lines)
    if cur is None:
        rep.add(S, "RESULT 줄", "FAIL", "없음")
    else:
        cases, checks, failed = cur
        rep.add(S, "failed_checks", "PASS" if failed == 0 else "FAIL", str(failed))
        if base is None:
            rep.add(S, "cases/checks", "INFO" if args.record else "FAIL", f"기준 없음, 이번 cases={cases} checks={checks}")
        else:
            for label, x, y in (("cases", base[0], cases), ("checks", base[1], checks)):
                if x == y:
                    rep.add(S, label, "PASS", f"{y} (기준과 같음)")
                elif y < x:
                    rep.add(S, label, "FAIL", f"줄어듦 {x} -> {y}")
                else:
                    rep.add(S, label, "FAIL" if args.exact_counts else "INFO", f"늘어남 {x} -> {y}")
    hit = [l for l in lines if re.search(TOOL_WARNING, l) or re.search(TOOL_ERROR, l)]
    rep.add(S, "컴파일 warning/error", "PASS" if not hit else "FAIL", f"{len(hit)}" + (": " + show(hit[0])[:160] if hit else ""))
    if b"Run build.bat before this test." in b"\n".join(lines):
        rep.add(S, "build\\*.obj", "FAIL", "build.bat을 먼저 돌려야 함")
    if code is None:
        rep.add(S, "종료 코드", "WARN", "로그에 exit= 줄이 없음")
    else:
        rep.add(S, "종료 코드", "PASS" if code == 0 else "FAIL", str(code))


# ---------------------------------------------------------------- f. hash lines
def hash_lines_of(lines):
    """The 'HASH: ' and 'UNSTABLE: ' lines of a log (stripped), in log order."""
    return [l.rstrip() for l in (lines or []) if HASH_RE.match(l) or UNSTABLE_RE.match(l)]   # at the very start of the line


def parse_hashes(lines):
    """({name: [hex, ...]}, {unstable names}) from hash lines; hex in lower case, a name's values in order."""
    vals, unstable = {}, set()
    for l in lines or []:
        s = l.rstrip()
        m = HASH_RE.match(s)
        if m:
            vals.setdefault(show(m.group(1)), []).append(m.group(2).decode().lower())
            continue
        m = UNSTABLE_RE.match(s)
        if m:
            unstable.add(show(m.group(1)))
    return vals, unstable


def prefix_of(name):
    return name.split("/", 1)[0] if "/" in name else "(접두어 없음)"


def prefix_counts(names):
    c = Counter(prefix_of(n) for n in names)
    return ", ".join(f"{p} {n}" for p, n in sorted(c.items())) or "0"


def check_hashes(rep: Report, cur_lines, base_lines, args, complete, trace_note=None, recording=False):
    """cur_lines: the hash lines of this run (regressions.log, then interactions.log); base_lines: the baseline's
    hash-lines.txt (None: an older baseline without one). complete: both logs were read this run (a section skipped
    -> a name missing from this run is SKIP, not FAIL)."""
    S = "hashes"
    cur, cur_unst = parse_hashes(cur_lines)
    dup = sorted(n for n, v in cur.items() if len(v) > 1)
    rep.add(S, "이번 HASH 줄", "INFO", f"{sum(len(v) for v in cur.values())}줄, 이름 {len(cur)}개 ({prefix_counts(cur)})"
            + (f"; UNSTABLE {len(cur_unst)}: " + ", ".join(sorted(cur_unst)[:6]) if cur_unst else ""))
    rep.add(S, "같은 이름 두 번", "PASS" if not dup else "FAIL",
            "없음" if not dup else f"{len(dup)}개: " + ", ".join(f"{n} x{len(cur[n])}" for n in dup[:args.list_limit]))
    if base_lines is None:
        rep.add(S, "기준 대조", "INFO", f"--record: 대조하지 않음 (이 줄들을 {HASH_FILE}에 씀)" if recording else
                f"기준 없음 (기준 폴더에 {HASH_FILE}가 없음: 0-3 전에 잡은 기준) - 대조하지 않음")
        return
    base, base_unst = parse_hashes(base_lines)
    bdup = sorted(n for n, v in base.items() if len(v) > 1)
    if bdup:
        rep.add(S, "기준의 같은 이름 두 번", "FAIL", f"{len(bdup)}개: " + ", ".join(bdup[:args.list_limit]))
    allow = tuple(args.allow_hash_change or ())
    unstable = cur_unst | base_unst
    same, diff, allowed, gone, new, skipped = 0, [], [], [], [], []
    for n in sorted(set(base) | set(cur) | unstable):
        if n in unstable:
            skipped.append(n)
            continue
        b, c = base.get(n), cur.get(n)
        if b and not c:
            gone.append(n)
        elif c and not b:
            new.append(n)
        elif b[0] == c[0]:
            same += 1
        elif n.startswith(allow) and allow:
            allowed.append((n, b[0], c[0]))
        else:
            diff.append((n, b[0], c[0]))
    rep.add(S, "기준 대조", "PASS" if not diff and not gone else "FAIL",
            f"기준 {len(base)}개 ({prefix_counts(base)}), 같음 {same}, 다름 {len(diff) + len(allowed)}, 사라짐 {len(gone)}, 새 이름 {len(new)}"
            + (f", UNSTABLE이라 뺌 {len(skipped)}" if skipped else ""))
    for n, b, c in diff[:args.list_limit]:
        rep.add(S, "  값이 다름", "FAIL", f"{n} (기준 {b}, 이번 {c})" + (trace_note(n) if trace_note else ""))
    for n, b, c in allowed[:args.list_limit]:
        rep.add(S, "  값이 다름(허용)", "WARN", f"{n} (기준 {b}, 이번 {c}; --allow-hash-change)" + (trace_note(n) if trace_note else ""))
    for n in gone[:args.list_limit]:
        rep.add(S, "  사라진 이름", "FAIL" if complete else "SKIP", n + ("" if complete else " (그 로그를 돌리지 않음)"))
    for n in new[:args.list_limit]:
        rep.add(S, "  새 이름", "INFO", f"{n} {cur[n][0]}")
    for n in skipped[:args.list_limit]:
        side = "양쪽" if n in cur_unst and n in base_unst else ("이번" if n in cur_unst else "기준")
        rep.add(S, "  UNSTABLE(뺌)", "INFO", f"{n} ({side} UNSTABLE)")
    if max(len(diff), len(allowed), len(gone), len(new), len(skipped)) > args.list_limit:
        rep.add(S, "  (이름 목록)", "INFO", f"앞의 {args.list_limit}개만 찍음 (--list-limit)")


def clear_traces():
    """Removes the frame traces an earlier run left next to the test exes, so the copy holds this run's only."""
    for p in (ROOT / "build" / "tests").rglob(TRACE_GLOB):
        try:
            p.unlink()
        except OSError:
            pass


def copy_traces(rep: Report, out: Path):
    """Copies build\\tests\\**\\trace-*.txt to out\\traces\\ (same sub folders), to diff when a dock_trace HASH differs."""
    import shutil
    src = ROOT / "build" / "tests"
    files = sorted(src.rglob(TRACE_GLOB))
    if not files:
        rep.add("hashes", "흔적 원문(trace-*.txt)", "INFO", "build\\tests 아래에 없음")
        return
    dst = out / "traces"
    total = 0
    for p in files:
        t = dst / p.relative_to(src)
        t.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(p, t)
        total += p.stat().st_size
    rep.add("hashes", "흔적 원문(trace-*.txt)", "INFO", f"{len(files)}개 ({total / 1e6:.1f} MB) -> {dst}")


def trace_noter(base_dir, out):
    """For a differing dock_trace/<scene>: the diff to run, when both sides kept their traces."""
    def note(name):
        if not name.startswith("dock_trace/") or not (base_dir and out):
            return ""
        f = f"trace-{name.split('/', 1)[1]}.txt"
        hits = [p for p in (out / "traces").rglob(f)] if (out / "traces").is_dir() else []
        bh = [p for p in (base_dir / "traces").rglob(f)] if (base_dir / "traces").is_dir() else []
        if hits and bh:
            return f"; diff {bh[0]} {hits[0]}"
        return "; 흔적 원문 " + ("이번 " + str(hits[0]) if hits else "") + (" 기준 " + str(bh[0]) if bh else "") if hits or bh else ""
    return note


# ---------------------------------------------------------------- d. dev compile, e. self-check
def build_cf():
    """CF of build.bat (with DEFS expanded), so the dev compile uses the same options."""
    bat = (ROOT / "build.bat").read_text(encoding="utf-8", errors="replace")
    defs = re.search(r'^set "DEFS=([^"]*)"', bat, re.M)
    cf = re.search(r'^set "CF=([^"]*)"', bat, re.M)
    if not (defs and cf):
        return "/nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE", False
    return cf.group(1).replace("%DEFS%", defs.group(1)), True


def dev_compile(rep: Report, out: Path):
    S = "dev-compile"
    cf, parsed = build_cf()
    dev = SCRATCH / "dev"
    dev.mkdir(parents=True, exist_ok=True)
    fo = os.path.relpath(dev, ROOT) + "\\"
    inner = f'call .\\tests\\vsenv.bat >nul && cl {cf} /DLD_DEV /c /Fo{fo} src\\main.cpp'
    log = out / "dev-compile.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    print(f"[gate] dev-compile: {inner}", flush=True)
    t0 = time.time()
    with open(log, "wb") as f:
        code = subprocess.run(f'cmd /d /s /c "{inner}"', cwd=ROOT, stdout=f, stderr=subprocess.STDOUT,
                              stdin=subprocess.DEVNULL, timeout=900).returncode
    with open(log, "ab") as f:
        f.write(f"exit={code}\r\n".encode())
    lines = read_lines(log)
    warn = [l for l in lines if re.search(TOOL_WARNING, l)]
    err = [l for l in lines if re.search(TOOL_ERROR, l)]
    obj = dev / "main.obj"
    ok = code == 0 and not warn and not err and obj.exists()
    rep.add(S, "src\\main.cpp /DLD_DEV", "PASS" if ok else "FAIL",
            f"exit={code}, warning {len(warn)}, error {len(err)}, {time.time() - t0:.0f} s, -> {obj.relative_to(ROOT)}"
            + ("" if parsed else " (build.bat의 CF를 못 읽어 기본 옵션)")
            + (": " + show((warn or err)[0])[:160] if warn or err else ""))


def self_check(rep: Report, out: Path, base_dir):
    S = "self-check"
    script = ROOT / "tools" / "llm-tasks" / "run_tasks.py"
    if not script.exists():
        rep.add(S, "run_tasks.py --self-check", "SKIP", "스크립트 없음")
        return None
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    r = subprocess.run([sys.executable, str(script), "--self-check"], cwd=ROOT, capture_output=True, env=env, timeout=600)
    data = r.stdout + r.stderr + f"exit={r.returncode}\r\n".encode()
    if out:
        out.mkdir(parents=True, exist_ok=True)
        (out / "selfcheck.log").write_bytes(data)

    def summary(lines):
        for l in reversed(lines or []):
            if "작업 팩 점검".encode() in l:
                return show(l.strip())
        return None
    cur = summary(lines_of(data))
    base = summary(read_lines(base_dir / "selfcheck.log")) if base_dir else None
    note = f"이번: {cur or '요약 줄 없음'} (exit={r.returncode})" + (f" / 기준: {base}" if base else " / 기준 selfcheck.log 없음")
    st = "INFO" if (base is None or base == cur) else "WARN"
    rep.add(S, "run_tasks.py --self-check", st, note + (" - 기준과 다름" if st == "WARN" else ""))
    return data


# ---------------------------------------------------------------- record
def write_hashes(out: Path, hashes):
    """out\\hash-after-build.txt in the baseline's 'sha256 *path' form (every live build writes it, so --from-logs on a
    gate folder still compares the helper's hash)."""
    out.mkdir(parents=True, exist_ok=True)
    (out / "hash-after-build.txt").write_bytes("".join(f"{hashes[e]} *{e}\n" for e in EXES if hashes.get(e)).encode())


def record(out: Path, build_lines, hashes, reg_lines, inter_lines):
    out.mkdir(parents=True, exist_ok=True)
    if hashes:
        write_hashes(out, hashes)
    if reg_lines is not None:
        res = [r[5] for r in extract_results(reg_lines)] + [l.strip() for l in reg_lines if l.strip() == FINAL]
        (out / "regressions-results.txt").write_bytes(b"".join(l + b"\n" for l in res))
        (out / "regressions-pass-lines.txt").write_bytes(b"".join(l + b"\n" for l in reg_lines if l.startswith(b"PASS")))
    if reg_lines is not None or inter_lines is not None:
        hl = hash_lines_of(reg_lines) + hash_lines_of(inter_lines)
        (out / HASH_FILE).write_bytes(b"".join(l + b"\n" for l in hl))
    write_test_exe_hashes(out)


def write_test_exe_hashes(out: Path):
    """out\\test-exe-hashes.txt: the sha256 of every test exe the bats build, in the baseline's 'sha256 *path' form and
    order (build/tests/*.exe sorted, then the sub folders in the order the bats use them). --record writes it into the
    baseline; a live run writes it into its --out, so the next step's --test-exe-hashes <that --out>\\test-exe-hashes.txt
    shows only the test exes that step changed."""
    out.mkdir(parents=True, exist_ok=True)
    exes = list(dict.fromkeys([e for _, e in bat_test_order()] + ["build/tests/dock-interaction/dock_interaction_test.exe"]))
    subs = list(dict.fromkeys(e.split("/")[2] for e in exes if e.count("/") == 3))
    exes.sort(key=lambda e: (0, "", e) if e.count("/") == 2 else (1, f"{subs.index(e.split('/')[2]):03}", e))
    hs = [(sha256(ROOT / e), e) for e in exes]
    (out / "test-exe-hashes.txt").write_bytes("".join(f"{h} *{e}\n" for h, e in hs if h).encode())


def compare_test_exes(rep: Report, base_dir: Path, path: Path = None):
    path = path or (base_dir / "test-exe-hashes.txt")
    base = read_hash_file(path)
    if not base:
        rep.add("tests", "테스트 exe 해시", "INFO", f"{path.name}: 해시 줄 없음 (판정에 넣지 않음)")
        return
    same, diff, missing = 0, [], []
    for e, h in base.items():
        cur = sha256(ROOT / e)
        if cur is None:
            missing.append(e)
        elif cur == h:
            same += 1
        else:
            diff.append(e)
    names = lambda es: ", ".join(e.rsplit("/", 1)[-1] for e in es)
    rep.add("tests", "테스트 exe 해시", "INFO", f"{path.name} {len(base)}개 가운데 같음 {same}, 다름 {len(diff)}, 없음 {len(missing)}"
            + (f"; 다름: {names(diff)}" if diff else "") + (f"; 없음: {names(missing)}" if missing else "") + " (판정에 넣지 않음)")


# ---------------------------------------------------------------- main
def print_report(rep: Report, table, out: Path, record_mode):
    lines = []
    if table:
        lines.append("")
        lines.append("테스트별 결과 줄 (기준 | 이번)")
        w = max(len(t[0]) for t in table)
        for t, b, c, st, why in table:
            lines.append(f"  {st:4} {t:<{w}}  {b:<34} | {c:<34} {'' if why == '같음' else why}")
    lines.append("")
    lines.append(f"{'구역':<13} {'판정':<5} 항목 - 내용")
    for sec, item, st, detail in rep.rows:
        if sec == "regressions" and item.startswith("결과 ") and st in ("PASS", "INFO") and table:
            continue        # already in the table above
        lines.append(f"{sec:<13} {st:<5} {item} - {detail}")
    fails = rep.fails()
    # rows of the judged sections that were not checked (SKIP) or only half checked (WARN): the PASS is then partial
    partial = [r for r in rep.rows if r[2] in ("SKIP", "WARN") and r[0] in JUDGED]
    lines.append("")
    if record_mode:
        lines.append("GATE: RECORDED" + (" (단, 절대 조건 실패: " + "; ".join(f"{f[0]}/{f[1]}" for f in fails[:8]) + ")" if fails else ""))
    elif not fails and not partial:
        lines.append("GATE: PASS")
    elif not fails:
        lines.append("GATE: PASS (부분: " + "; ".join(f"{r[0]}/{r[1]} {r[2]}" for r in partial[:12])
                     + (f"; ... 그 밖 {len(partial) - 12}개" if len(partial) > 12 else "") + ")")
    else:
        reasons = [f"{f[0]}/{f[1]}: {f[3]}" for f in fails]
        lines.append("GATE: FAIL (" + "; ".join(reasons[:12]) + (f"; ... 그 밖 {len(reasons) - 12}개" if len(reasons) > 12 else "") + ")")
    text = "\n".join(lines)
    print(text)
    if out:
        out.mkdir(parents=True, exist_ok=True)
        (out / "gate-summary.txt").write_text(text + "\n", encoding="utf-8")
    if fails:
        return 1
    return 3 if partial and not record_mode else 0


def main():
    ap = argparse.ArgumentParser(description="리팩토링 관문 실행기 (tools/refactor/README.md)")
    ap.add_argument("--baseline", type=Path, help="기준 로그 폴더 (hash-after-build.txt, regressions-results.txt, "
                    "regressions-pass-lines.txt, interactions.log). --record가 아니면 필수")
    ap.add_argument("--out", type=Path, help="이번 실행의 로그·요약을 둘 폴더 (--record면 새 기준 폴더)")
    ap.add_argument("--from-logs", type=Path, help="돌리지 않고 이 폴더의 build.log, regressions.log, interactions.log"
                    "(그리고 있으면 hash-after-build.txt)를 대조만")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-regressions", action="store_true")
    ap.add_argument("--skip-interactions", action="store_true")
    ap.add_argument("--skip-self-check", action="store_true")
    ap.add_argument("--dev-compile", action="store_true", help="src\\main.cpp를 /DLD_DEV로 build\\refactor-scratch\\dev\\에 컴파일")
    ap.add_argument("--record", action="store_true", help="대조하지 않고 --out에 기준 파일을 만든다")
    ap.add_argument("--exact-counts", action="store_true", help="검사 수가 늘어도 FAIL (늘리지 않는 단계)")
    ap.add_argument("--require-same-exe", action="store_true", help="LiquidDock.exe·Setup 해시가 달라도 FAIL (해시가 같아야 하는 단계)")
    ap.add_argument("--allow-helper-change", action="store_true", help="도우미 해시가 달라도 경고만 (버전을 올리는 단계)")
    ap.add_argument("--rerun-failed", action="store_true", help="멈춘 테스트부터 끝까지의 exe를 한 번 더 직접 돌려 흔들림을 가린다")
    ap.add_argument("--list-limit", type=int, default=40, help="사라진/새 PASS 줄을 몇 개까지 찍을지")
    ap.add_argument("--allow-no-hash", action="store_true", help="--from-logs 폴더에 hash-after-build.txt가 없어도 FAIL "
                    "대신 SKIP (그러면 끝 줄은 'GATE: PASS (부분: ...)')")
    ap.add_argument("--test-exe-hashes", type=Path, help="테스트 exe 해시를 견줄 파일 (기본: 기준 폴더의 test-exe-hashes.txt; "
                    "예: test-exe-hashes-after-0-2.txt). 판정에는 넣지 않음")
    ap.add_argument("--allow-hash-change", action="append", metavar="접두어", help="이 접두어로 시작하는 HASH 이름(예: "
                    "dock_pixels/, settings_pixels/s150-)은 값이 달라도 FAIL 대신 WARN (그림이 바뀌어도 되는 단계; 여러 번 줄 수 있음)")
    ap.add_argument("--force", action="store_true", help="--record가 --out에 이미 있는 기준 파일·로그를 덮어도 됨")
    args = ap.parse_args()

    if not args.record and not args.baseline:
        ap.error("--baseline이 필요합니다 (기준을 새로 잡으려면 --record --out <폴더>)")
    if args.record and not args.out:
        ap.error("--record에는 --out이 필요합니다")
    if args.baseline and not args.baseline.is_dir():
        ap.error(f"기준 폴더가 없습니다: {args.baseline}")
    if args.from_logs and not args.from_logs.is_dir():
        ap.error(f"로그 폴더가 없습니다: {args.from_logs}")
    out = args.out.resolve() if args.out else None
    if not out and not args.from_logs:
        out = SCRATCH / time.strftime("gate-%Y%m%d-%H%M%S")
    base_dir = args.baseline.resolve() if args.baseline else None
    live = args.from_logs is None
    rep = Report()

    # the baseline folder is not in git (userResult/ is ignored): never write into it
    if out and base_dir and out == base_dir:
        ap.error(f"--out이 기준 폴더와 같습니다: {out} (기준 로그를 덮게 됨; 다른 --out을 주세요)")
    if args.record and out and out.is_dir() and not args.force:
        names = list(BASELINE_FILES) + (list(LOG_FILES) if live else [])
        there = [n for n in names if (out / n).exists()]
        if there:
            ap.error(f"--record: {out}에 이미 {', '.join(there)}이(가) 있습니다 (덮으려면 --force, 아니면 빈 폴더)")

    if out:
        print(f"[gate] 로그 폴더: {out}")
    base = {}
    if base_dir:
        base["hashes"] = read_hash_file(base_dir / "hash-after-build.txt")
        base["results"] = read_lines(base_dir / "regressions-results.txt")
        base["pass"] = read_lines(base_dir / "regressions-pass-lines.txt")
        base["inter"] = read_lines(base_dir / "interactions.log")
        base["hash"] = read_lines(base_dir / HASH_FILE)        # None: a baseline from before 0-3 (INFO '기준 없음')
    if not args.record:
        # every section that is checked needs its baseline file; without it the comparison would silently be empty
        missing = []
        if not args.skip_build:
            missing += [f"hash-after-build.txt의 {e}" for e in EXES if e not in base["hashes"]]
        if not args.skip_regressions:
            if not base["results"] or not extract_results(base["results"]):
                missing.append("regressions-results.txt(결과 줄)")
            if not base["pass"]:
                missing.append("regressions-pass-lines.txt(PASS 줄)")
        if not args.skip_interactions and result_of(base["inter"]) is None:
            missing.append("interactions.log(RESULT 줄)")
        if missing:
            ap.error(f"기준 파일 없음 ({base_dir}): " + ", ".join(missing)
                     + " - 기준 폴더가 맞는지 보세요(새로 잡으려면 --record)")

    build_lines = reg_lines = inter_lines = hashes = None
    build_ok = True
    table = None

    # a. build
    if not args.skip_build:
        if live:
            code, _ = run_logged(["cmd", "/d", "/c", ".\\build.bat"], out / "build.log", 1800, "build.bat")
            build_lines = read_lines(out / "build.log")
            hashes = {e: sha256(ROOT / e) for e in EXES}           # after build.bat has exited
            write_hashes(out, hashes)                               # so --from-logs on this folder sees them too
            check_stale_obj(rep)
        else:
            build_lines = read_lines(args.from_logs / "build.log")
            code = exit_code_in(build_lines)
            hf = args.from_logs / "hash-after-build.txt"
            hashes = read_hash_file(hf) if hf.exists() else None
            rep.add("build", "옛 build\\*.obj", "SKIP", "--from-logs: 작업 트리의 build\\는 보지 않음")
        check_build(rep, build_lines, code, hashes, base.get("hashes"), args)
        build_ok = build_lines is not None and code == 0
    else:
        rep.add("build", "build.bat", "SKIP", "--skip-build")

    # b. regressions
    if args.skip_regressions:
        rep.add("regressions", "run_regressions", "SKIP", "--skip-regressions")
    elif live and not build_ok:
        rep.add("regressions", "run_regressions", "FAIL", "build.bat이 실패해 돌리지 않음")
    else:
        if live:
            clear_traces()
            code, _ = run_logged(["cmd", "/d", "/c", ".\\tests\\run_regressions.bat", "--logged"], out / "regressions.log",
                                 3600, "run_regressions")
            reg_lines = read_lines(out / "regressions.log")
            copy_traces(rep, out)
        else:
            reg_lines = read_lines(args.from_logs / "regressions.log")
            code = exit_code_in(reg_lines)
        r = check_regressions(rep, reg_lines, code, base.get("results"), base.get("pass"), args)
        if r:
            table, cur = r
            if args.rerun_failed:
                rerun(rep, rerun_candidates(reg_lines, cur), out or Path("."), live)

    # c. interactions
    if args.skip_interactions:
        rep.add("interactions", "run_dock_interactions", "SKIP", "--skip-interactions")
    elif live and not build_ok:
        rep.add("interactions", "run_dock_interactions", "FAIL", "build.bat이 실패해 돌리지 않음 (build\\*.obj가 필요)")
    else:
        if live:
            code, _ = run_logged(["cmd", "/d", "/c", ".\\tests\\run_dock_interactions.bat"], out / "interactions.log",
                                 3600, "run_dock_interactions")
            inter_lines = read_lines(out / "interactions.log")
        else:
            inter_lines = read_lines(args.from_logs / "interactions.log")
            code = exit_code_in(inter_lines)
        check_interactions(rep, inter_lines, code, base.get("inter"), args)

    # f. hash lines of the characterization tests (both logs), against the baseline's hash-lines.txt
    if reg_lines is None and inter_lines is None:
        rep.add("hashes", "HASH 줄", "SKIP", "regressions·interactions 로그를 읽지 않음")
    else:
        check_hashes(rep, hash_lines_of(reg_lines) + hash_lines_of(inter_lines), base.get("hash") if base_dir else None,
                     args, complete=reg_lines is not None and inter_lines is not None,
                     trace_note=trace_noter(base_dir, args.from_logs.resolve() if args.from_logs else out),
                     recording=args.record and base_dir is None)

    # d, e
    if args.dev_compile:
        dev_compile(rep, out or (SCRATCH / "dev"))
    if not args.skip_self_check:
        self_check(rep, out, base_dir)
    if live and base_dir and not (args.skip_regressions and args.skip_interactions):
        compare_test_exes(rep, base_dir, args.test_exe_hashes)
    if live and out and not args.record and not (args.skip_regressions and args.skip_interactions):
        # after the comparison (--test-exe-hashes may name this very file from an earlier run into the same --out)
        write_test_exe_hashes(out)
        rep.add("tests", "테스트 exe 해시 파일", "INFO", f"{out / 'test-exe-hashes.txt'} (다음 단계의 --test-exe-hashes; 판정에 넣지 않음)")

    if args.record:
        if not live:
            for name, ls in (("build.log", build_lines), ("regressions.log", reg_lines), ("interactions.log", inter_lines)):
                if ls is not None and (args.from_logs.resolve() / name) != (out / name):
                    out.mkdir(parents=True, exist_ok=True)
                    (out / name).write_bytes((args.from_logs / name).read_bytes())
        record(out, build_lines, hashes, reg_lines, inter_lines)
        print(f"[gate] 기준 파일을 썼습니다: {out}")
    return print_report(rep, table, out, args.record)


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace"); sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    sys.exit(main())
