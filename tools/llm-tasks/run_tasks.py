#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Liquid Dock - 로컬 LLM 작업 실행기.

tasks.json의 작업마다 로컬 LLM(Ollama, LM Studio, llama.cpp server, vLLM ...)에 프롬프트를 보내고, 답에서
코드 블록을 꺼내 out\<작업>\에 저장한 뒤 검사합니다. C++ 테스트는 빌드해서 돌리고, 일부러 망가뜨린 코드
(변이, mutant)로도 돌려 그 망가짐을 잡아내는지 봅니다. 검사에 실패하면 실패 내용을 모델에 돌려주고 다시
받습니다(--max-attempts). Python 3.8 이상, 표준 라이브러리만 씁니다.

  python tools\llm-tasks\run_tasks.py --list
  python tools\llm-tasks\run_tasks.py --model qwen2.5-coder:14b                 (Ollama, 모든 작업)
  python tools\llm-tasks\run_tasks.py --model qwen2.5-coder:14b --tasks T3      (작업 하나)
  python tools\llm-tasks\run_tasks.py --api openai --base-url http://127.0.0.1:1234/v1 --model <이름>
                                                                                (LM Studio, llama.cpp server ...)
  python tools\llm-tasks\run_tasks.py --verify-only      (모델 호출 없이 out\의 결과만 다시 검사)
  python tools\llm-tasks\run_tasks.py ... --apply        (통과한 결과를 저장소의 제자리에 복사)
  python tools\llm-tasks\run_tasks.py --dry-run          (프롬프트만 out\에 저장)
  python tools\llm-tasks\run_tasks.py --self-check       (작업 팩 점검: 문맥 파일, 변이 위치, 검사 스크립트)

생성된 코드는 이 PC에서 빌드·실행됩니다. 위험한 호출(파일 쓰기·삭제, 프로세스 실행, 네트워크, 레지스트리)이
보이면 실행하지 않고 모델에 다시 받습니다. --apply 전에는 결과를 꼭 읽어 보세요.
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent            # the repository
OUT = HERE / "out"

for _stream in (sys.stdout, sys.stderr):   # a console or file that cannot show a character must not stop the run
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass


def load_tasks():
    with open(HERE / "tasks.json", encoding="utf-8") as f:
        return json.load(f)["tasks"]


def tail(text, n=60):
    lines = (text or "").splitlines()
    return "\n".join(lines[-n:])


# ---------------------------------------------------------------- the prompt

LANG_BY_SUFFIX = {".h": "cpp", ".cpp": "cpp", ".py": "python", ".md": "markdown", ".json": "json", ".log": "text", ".txt": "text"}


def expand(text, args):
    return (text.replace("{python}", sys.executable).replace("{root}", str(ROOT))
                .replace("{diff_base}", getattr(args, "diff_base", "HEAD")))


def section(text, start=None, end=None, end_occurrence=1):
    """The lines from the first one starting with `start` up to (not including) the `end_occurrence`-th one
    starting with `end`."""
    out, on, seen = [], start is None, 0
    for line in text.splitlines():
        if not on:
            if line.startswith(start):
                on = True
            else:
                continue
        if end is not None and line.startswith(end):
            seen += 1
            if seen >= end_occurrence:
                break
        out.append(line)
    return "\n".join(out)


def limit_lines(text, max_lines):
    lines = text.splitlines()
    if max_lines and len(lines) > max_lines:
        return "\n".join(lines[:max_lines]) + f"\n... ({len(lines) - max_lines} more lines not shown)"
    return text


def context_blocks(task, args):
    """[(title, lang, text)], or (None, why) when a context marked skip_if_empty is empty."""
    blocks = []
    for item in task.get("context", []):
        if "file" in item:
            path = ROOT / item["file"]
            text = path.read_text(encoding="utf-8-sig", errors="replace")
            lang = item.get("lang", LANG_BY_SUFFIX.get(path.suffix, ""))
        else:
            cmd = [expand(a, args) for a in item["cmd"]]
            try:
                r = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=120)
            except (OSError, subprocess.TimeoutExpired) as e:
                raise RuntimeError(f"문맥 명령을 실행할 수 없습니다 ({' '.join(cmd)}): {e}")
            if r.returncode != 0:
                raise RuntimeError(f"문맥 명령이 실패했습니다 ({' '.join(cmd)}): "
                                   + tail(r.stderr.decode("utf-8", "replace"), 5))
            text = r.stdout.decode("utf-8", "replace")
            lang = item.get("lang", "")
        if "from" in item or "to" in item:
            text = section(text, item.get("from"), item.get("to"), item.get("to_occurrence", 1))
        text = limit_lines(text, item.get("max_lines"))
        if item.get("skip_if_empty") and not text.strip():
            return None, expand(item.get("empty_note", "문맥이 비어 있습니다"), args)
        blocks.append((expand(item.get("title", item.get("file", " ".join(item.get("cmd", [])))), args), lang, text))
    return blocks, None


def fence_for(text):
    """A code fence longer than any backtick run inside the text."""
    longest = max((len(m) for m in re.findall(r"`{3,}", text)), default=0)
    return "`" * max(3, longest + 1)


def estimate_tokens(text):
    """A rough upper estimate: about 3.5 characters per token for code and English, one per other character
    (Korean, symbols)."""
    ascii_chars = sum(1 for ch in text if ord(ch) < 128)
    return int(ascii_chars / 3.5) + (len(text) - ascii_chars)


def build_prompt(task, args):
    blocks, skip = context_blocks(task, args)
    if blocks is None:
        return None, skip
    parts = [(HERE / "prompts" / "common_rules.md").read_text(encoding="utf-8").strip(),
             (HERE / task["prompt"]).read_text(encoding="utf-8").strip()]
    for title, lang, text in blocks:
        f = fence_for(text)
        parts.append(f"### {title}\n{f}{lang}\n{text.rstrip()}\n{f}")
    parts.append(f"Now answer with the complete file `{task['target']}` in one code block.")
    return "\n\n".join(parts), None


# ---------------------------------------------------------------- the model

def call_llm(args, messages):
    if args.api == "ollama":
        # Ollama's own API: the only way to set the context size per request (its OpenAI-style API keeps
        # the server's default, often 4096 tokens, and silently cuts longer prompts)
        url = args.base_url.rstrip("/") + "/api/chat"
        options = {"temperature": args.temperature, "num_ctx": args.num_ctx}
        if args.max_tokens:
            options["num_predict"] = args.max_tokens
        payload = {"model": args.model, "messages": messages, "stream": False, "options": options}
    else:
        url = args.base_url.rstrip("/") + "/chat/completions"
        payload = {"model": args.model, "messages": messages, "temperature": args.temperature, "stream": False}
        if args.max_tokens:
            payload["max_tokens"] = args.max_tokens
    headers = {"Content-Type": "application/json"}
    if args.api_key:
        headers["Authorization"] = "Bearer " + args.api_key
    req = urllib.request.Request(url, data=json.dumps(payload).encode("utf-8"), headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=args.timeout) as resp:
            reply = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:400]
        hint = f" (모델이 없으면: ollama pull {args.model})" if args.api == "ollama" and e.code == 404 else ""
        raise RuntimeError(f"API 오류 {e.code}: {detail}{hint}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"API에 연결할 수 없습니다 ({url}): {e.reason} - 서버(ollama serve, LM Studio 서버)가 켜져 있나요?")
    except (TimeoutError, OSError) as e:
        raise RuntimeError(f"API 응답이 없습니다 ({url}, {args.timeout}초): {e}")
    try:
        content = reply["message"]["content"] if args.api == "ollama" else reply["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        raise RuntimeError("API 응답 형식이 예상과 다릅니다: " + json.dumps(reply, ensure_ascii=False)[:300])
    return re.sub(r"<think>.*?</think>", "", content or "", flags=re.S).strip()   # reasoning models think aloud first


FENCE = re.compile(r"^(`{3,})[ \t]*([A-Za-z0-9_+\-]*)[^\n]*\n(.*?)^\1[ \t]*$", re.S | re.M)
LANG_TAGS = {"cpp": {"cpp", "c++", "cxx", "cc", "c"}, "python": {"python", "py", "python3"}, "markdown": {"markdown", "md"}}


def extract_block(text, lang):
    """The code block holding the answer (the longest one in the wanted language)."""
    if lang == "markdown":
        # a document may hold code blocks of its own: up to the last closing fence of the same length
        m = re.search(r"^(`{3,})[ \t]*(?:markdown|md)[^\n]*\n(.*)^\1[ \t]*$", text, re.S | re.M)
        if m:
            return m.group(2).rstrip() + "\n"
        if text.lstrip().startswith("#"):
            return text.strip() + "\n"
    blocks = [(m.group(2).lower(), m.group(3)) for m in FENCE.finditer(text)]
    if not blocks:
        return None
    tagged = [b for t, b in blocks if t in LANG_TAGS.get(lang, {lang})]
    return max(tagged or [b for _, b in blocks], key=len).rstrip() + "\n"


UNSAFE = {   # (pattern, what it is): generated code that does any of this is not run
    "cpp": [(r"\bsystem\s*\(", "system()"), (r"\b_?w?popen\s*\(", "popen()"), (r"\bCreateProcess", "CreateProcess"),
            (r"\bShellExecute", "ShellExecute"), (r"\bWinExec\b", "WinExec"), (r"std::filesystem", "std::filesystem"),
            (r"\b_?w?remove\s*\(\s*L?\"", "remove(\"file\")"), (r"\bunlink\s*\(", "unlink()"), (r"\bDeleteFile", "DeleteFile"),
            (r"\bMoveFile", "MoveFile"), (r"\bReg(Set|Delete|Create)", "registry writes"), (r"\b_?w?fopen(_s)?\s*\(", "fopen()"),
            (r"\bw?[io]?fstream\b", "file streams"), (r"\bsocket\s*\(", "socket()"), (r"\bWSAStartup\b", "WSAStartup"),
            (r"\bInternetOpen", "InternetOpen"), (r"\bURLDownload", "URLDownload"), (r"#\s*include\s*[<\"]windows\.h[>\"]", "#include <windows.h>")],
    "python": [(r"\bsubprocess\b", "subprocess"), (r"\bos\.(system|popen|remove|unlink|rmdir|removedirs|rename|replace|exec\w*|spawn\w*|kill)\b",
                "os.system/remove/rename ..."), (r"\bshutil\b", "shutil"), (r"\bsocket\b", "socket"), (r"\burllib\b", "urllib"),
               (r"\brequests\b", "requests"), (r"\bhttp\.client\b", "http.client"), (r"\bctypes\b", "ctypes"), (r"\bwinreg\b", "winreg"),
               (r"\bopen\([^)]*,\s*(mode\s*=\s*)?['\"][^'\"]*[wax+]", "open() for writing"), (r"\.write_(text|bytes)\(", "write_text/write_bytes"),
               (r"\.(unlink|rmdir)\(", "unlink/rmdir"), (r"\bexec\s*\(", "exec()"), (r"\beval\s*\(", "eval()"), (r"__import__", "__import__"),
               (r"\bimportlib\b", "importlib")],
    "markdown": [],
}


def unsafe_hits(code, lang):
    return [name for pattern, name in UNSAFE.get(lang, []) if re.search(pattern, code)]


# ---------------------------------------------------------------- C++ compiler

def env_get(env, name):
    for k, v in env.items():
        if k.upper() == name.upper():
            return v
    return None


def find_vcvars():
    """vcvars64.bat the way tests/vsenv.bat finds it: the usual Build Tools path, then vswhere."""
    fixed = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
    if fixed.exists():
        return fixed
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        r = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                            "-property", "installationPath"], capture_output=True, text=True)
        p = Path(r.stdout.strip()) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        if r.stdout.strip() and p.exists():
            return p
    return None


def vcvars_env(vcvars):
    """The environment vcvars64.bat sets up, read once (cmd /u prints it as UTF-16)."""
    r = subprocess.run(f'cmd /d /u /s /c ""{vcvars}" >nul 2>&1 && set"', capture_output=True, timeout=180)
    env = {}
    for line in r.stdout.decode("utf-16-le", "replace").splitlines():
        k, sep, v = line.partition("=")
        if sep and k:
            env[k] = v
    if not env_get(env, "INCLUDE"):
        raise RuntimeError(f"{vcvars}가 C++ 환경을 만들지 못했습니다")
    return env


class Toolchain:
    """cl (Visual Studio C++ tools), g++, clang++ or zig c++ ('pip install ziglang')."""

    def __init__(self, choice):
        order = [choice] if choice != "auto" else (["msvc", "g++", "clang++", "zig"] if os.name == "nt" else ["g++", "clang++", "zig"])
        self.kind, self.cmd, self.env, self.name = None, None, None, ""
        for c in order:
            if self._try(c):
                return
        raise RuntimeError("C++ 컴파일러가 없습니다: Visual Studio 2022 Build Tools(C++), g++, clang++ 중 하나를 설치하거나 "
                           "'pip install ziglang' 후 --cxx zig 를 쓰세요.")

    def _try(self, c):
        if c == "msvc" and os.name == "nt":
            cl = shutil.which("cl")
            if cl:
                self.kind, self.cmd, self.env, self.name = "msvc", [cl], dict(os.environ, VSLANG="1033"), "MSVC cl"
                return True
            vcvars = find_vcvars()
            if not vcvars:
                return False
            env = vcvars_env(vcvars)
            cl = shutil.which("cl", path=env_get(env, "PATH"))
            if not cl:
                return False
            env["VSLANG"] = "1033"   # English messages: easier for the model, and no code page trouble
            self.kind, self.cmd, self.env, self.name = "msvc", [cl], env, "MSVC cl (vcvars64)"
            return True
        if c in ("g++", "clang++"):
            exe = shutil.which(c)
            if exe:
                self.kind, self.cmd, self.name = "gnu", [exe], c
                return True
        if c == "zig":
            try:
                import ziglang  # noqa: F401
            except ImportError:
                return False
            self.kind, self.cmd, self.name = "gnu", [sys.executable, "-m", "ziglang", "c++"], "zig c++"
            return True
        return False

    def build(self, sources, exe, includes, win_libs, workdir):
        if self.kind == "msvc":
            args = [*self.cmd, "/nologo", "/std:c++20", "/EHsc", "/Od", "/utf-8", "/W3", "/DUNICODE", "/D_UNICODE"]
            args += [f"/I{d}" for d in includes] + [str(s) for s in sources] + [f"/Fe{exe}", f"/Fo{workdir}{os.sep}"]
            if win_libs:
                args += ["/link"] + [lib + ".lib" for lib in win_libs]
        else:
            args = [*self.cmd, "-std=c++20", "-O0", "-Wall"]
            for d in includes:
                args += ["-I", str(d)]
            args += [str(s) for s in sources] + ["-o", str(exe)]
            if os.name == "nt":
                args += [f"-l{lib}" for lib in win_libs]
        try:
            r = subprocess.run(args, cwd=workdir, env=self.env, capture_output=True, timeout=300)
        except subprocess.TimeoutExpired:
            return False, "the build did not finish in 300 s"
        out = (r.stdout + r.stderr).decode("utf-8", "replace")
        return r.returncode == 0 and Path(exe).exists(), out


def run_exe(exe, cwd, timeout):
    try:
        r = subprocess.run([str(exe)], cwd=cwd, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, f"it did not finish in {timeout} s (an endless loop?)"
    return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace")


# ---------------------------------------------------------------- checks
# Each returns (passed, a short Korean note for the report, the details for the model in English).

def check_cpp(task, cand, args, tc):
    v = task["verify"]
    work = cand.parent / "build"
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ("test.exe" if os.name == "nt" else "test")
    if exe.exists():
        exe.unlink()
    ok, out = tc.build([cand] + [ROOT / s for s in v.get("extra_sources", [])], exe, [ROOT], v.get("win_libs", []), work)
    if not ok:
        return False, "빌드 실패", "The build failed:\n" + tail(out, 60)
    rc, out = run_exe(exe, work, args.run_timeout)
    if rc is None:
        return False, "실행 시간 초과", "The test " + out
    m = re.search(r"\((\d+) checks", out)
    checks = int(m.group(1)) if m else 0
    if rc != 0 or "ALL PASSED" not in out:
        fails = [l for l in out.splitlines() if l.startswith("FAIL")]
        return False, f"테스트 실패 ({len(fails)}개)", (
            f"The test failed (exit code {rc}). Failing checks:\n" + ("\n".join(fails[:40]) or tail(out, 40))
            + "\nFor each failing check, read the code under test again: if your expectation was wrong, correct it; "
              "if you are sure the code is wrong, turn the check into a // SUSPECT: comment.")
    if checks < v.get("min_checks", 0):
        return False, f"검사 {checks}개 (최소 {v['min_checks']}개)", (
            f"The test passed but has only {checks} checks; at least {v['min_checks']} are required. Add more distinct checks.")
    note, detail = f"검사 {checks}개", f"The test passed with {checks} checks."
    if v.get("mutants") and not args.no_mutants:
        ok, mnote, mdetail = check_mutants(task, cand, args, tc, work)
        note, detail = note + ", " + mnote, detail + "\n" + mdetail
        if not ok:
            return False, note, detail
    return True, note, detail


def check_mutants(task, cand, args, tc, work):
    """Runs the test against copies of the code with one deliberate bug each: a test that still passes
    did not notice that bug."""
    v = task["verify"]
    mutants = v["mutants"]
    files = sorted({m["file"] for m in mutants} | set(v.get("extra_sources", [])) | set(v.get("copy", [])) | {"tests/portable_check.h"})
    killed, applied, survivors, broken = 0, 0, [], []
    for m in mutants:
        mdir = work / "mutants" / m["id"]
        if mdir.exists():
            shutil.rmtree(mdir)
        for f in files:
            (mdir / f).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / f, mdir / f)
        target = mdir / m["file"]
        text = target.read_text(encoding="utf-8")
        if text.count(m["find"]) != 1:
            broken.append(f"{m['id']} (no longer applies: {m['file']} changed)")
            continue
        target.write_text(text.replace(m["find"], m["replace"]), encoding="utf-8")
        exe = mdir / ("test.exe" if os.name == "nt" else "test")
        ok, out = tc.build([cand] + [mdir / s for s in v.get("extra_sources", [])], exe, [mdir, ROOT], v.get("win_libs", []), mdir)
        if not ok:
            broken.append(f"{m['id']} (does not build)")
            continue
        applied += 1
        rc, out = run_exe(exe, mdir, min(args.run_timeout, 30))
        if rc is None or rc != 0 or "ALL PASSED" not in out:
            killed += 1
        else:
            survivors.append(m)
    need = math.ceil(v.get("min_kill_ratio", 0.75) * applied)
    note = f"변이 {killed}/{applied}개 잡음" + (f" (필요 {need}개)" if killed < need else "")
    if broken:
        note += f", 적용 안 된 변이 {len(broken)}개"
    detail = f"Mutation check: your test noticed {killed} of {applied} deliberately broken versions of the code ({need} needed)."
    if survivors:
        detail += ("\nThese broken versions passed your test unnoticed. Add checks that pass on the real code "
                   "and would fail on each of them:\n" + "\n".join(f"- {m['desc']}" for m in survivors))
    if broken:
        detail += "\n(not counted: " + ", ".join(broken) + ")"
    return killed >= need, note, detail


def check_script(task, cand, args):
    cmd = [expand(a, args).replace("{candidate}", str(cand)) for a in task["verify"]["cmd"]]
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=max(args.run_timeout, 120))
    except subprocess.TimeoutExpired:
        return False, "검사 시간 초과", "The check did not finish in time."
    out = (r.stdout + r.stderr).decode("utf-8", "replace")
    last = next((l for l in reversed(out.splitlines()) if l.strip()), "")
    return r.returncode == 0, last or ("통과" if r.returncode == 0 else "검사 실패"), tail(out, 80)


def verify(task, cand, args, tc):
    kind = task["verify"]["kind"]
    if kind == "none":
        return True, "자동 검사 없음 (사람이 읽고 판단)", ""
    if kind == "cpp-test":
        return check_cpp(task, cand, args, tc)
    if kind == "script":
        return check_script(task, cand, args)
    return False, f"알 수 없는 검사 종류 {kind}", ""


# ---------------------------------------------------------------- one task

SYSTEM = ("You are a careful senior C++ and Python engineer. You follow the instructions exactly, and you derive "
          "every expectation from the code and comments you are given.")


def candidate_path(task):
    return OUT / task["id"] / Path(task["target"]).name


def run_task(task, args, tc, log):
    tdir = OUT / task["id"]
    tdir.mkdir(parents=True, exist_ok=True)
    cand = candidate_path(task)
    res = {"id": task["id"], "title": task["title"], "attempts": 0, "passed": False, "note": "", "seconds": 0.0}
    started = time.time()
    try:
        if args.verify_only:
            if not cand.exists():
                res["note"] = "out에 결과가 없습니다"
                return res
            ok, note, detail = verify(task, cand, args, tc)
            res.update(passed=ok, note=note, unchecked=task["verify"]["kind"] == "none")
            (tdir / "verify.txt").write_text(detail, encoding="utf-8")
            return res
        prompt, skip = build_prompt(task, args)
        if prompt is None:
            res.update(note=skip, skipped=True)
            return res
        (tdir / "prompt.md").write_text(prompt, encoding="utf-8")
        tokens = estimate_tokens(prompt)
        if args.dry_run:
            res["note"] = f"프롬프트 {len(prompt):,}자 (약 {tokens:,} 토큰 이하) → out/{task['id']}/prompt.md"
            return res
        if args.api == "ollama" and tokens * 1.6 > args.num_ctx:   # room for the answer and one retry
            log(f"  주의: 프롬프트가 약 {tokens:,} 토큰입니다. 답과 재시도가 --num-ctx {args.num_ctx}를 넘을 수 있으니 "
                f"--num-ctx {max(24576, int(tokens * 2.2) // 1024 * 1024)} 이상을 권합니다.")
        messages = [{"role": "system", "content": SYSTEM}, {"role": "user", "content": prompt}]
        detail = ""
        for attempt in range(1, args.max_attempts + 1):
            res["attempts"] = attempt
            log(f"  시도 {attempt}/{args.max_attempts}: 모델이 답하는 중...")
            t0 = time.time()
            answer = call_llm(args, messages)
            (tdir / f"answer-{attempt}.md").write_text(answer, encoding="utf-8")
            code = extract_block(answer, task["lang"])
            if code is None:
                note, detail = "코드 블록 없음", "Your answer had no code block. Answer with the complete file in one fenced code block."
            elif not args.allow_unsafe and unsafe_hits(code, task["lang"]):
                hits = unsafe_hits(code, task["lang"])
                note = "허용되지 않는 호출"
                detail = ("The file uses something that is not allowed here (files, processes, network, registry or Windows "
                          "headers): " + ", ".join(hits) + ". Remove it; compute and compare only.")
            else:
                cand.write_text(code, encoding="utf-8")
                ok, note, detail = verify(task, cand, args, tc)
                if ok:
                    res.update(passed=True, note=note, unchecked=task["verify"]["kind"] == "none")
                    break
            log(f"  실패 ({time.time() - t0:.0f}초): {note}")
            res["note"] = note
            # only the latest answer and its check go back, so the conversation stays inside the context size
            messages = messages[:2] + [{"role": "assistant", "content": answer},
                                       {"role": "user", "content": "The result did not pass the check. Fix it and answer again "
                                        "with the complete file in one code block. Never weaken a correct check just to make "
                                        f"it pass.\n\nCheck output:\n```\n{detail[:6000]}\n```"}]
        (tdir / "verify.txt").write_text(detail, encoding="utf-8")
        if res["passed"] and args.apply and task.get("apply", True):
            target = ROOT / task["target"]
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(cand, target)
            res["note"] += f" → {task['target']}에 반영"
        if res["passed"] and cand.exists():
            suspects = sum("SUSPECT:" in l for l in cand.read_text(encoding="utf-8").splitlines())
            if suspects:
                res["note"] += f" (SUSPECT {suspects}개: 코드 문제일 수 있으니 확인)"
    except RuntimeError as e:
        res["note"] = str(e)
    finally:
        res["seconds"] = round(time.time() - started, 1)
    return res


def status(r, args):
    if r.get("skipped"):
        return "건너뜀"
    if r.get("unchecked"):
        return "완료"
    if r["passed"]:
        return "통과"
    return "-" if args.dry_run else "실패"


def write_report(results, args, tc):
    lines = ["# 로컬 LLM 작업 결과", "",
             f"- 모델: `{args.model}` ({args.api}, {args.base_url}" + (f", num_ctx {args.num_ctx}" if args.api == "ollama" else "") + ")",
             f"- 컴파일러: {tc.name if tc else '-'}",
             f"- 시각: {time.strftime('%Y-%m-%d %H:%M:%S')}", "",
             "| 작업 | 내용 | 결과 | 시도 | 시간(초) | 메모 |", "|---|---|---|---|---|---|"]
    for r in results:
        lines.append(f"| {r['id']} | {r['title']} | {status(r, args)} | {r['attempts']} | {r['seconds']} | {r['note'].replace('|', '/')} |")
    lines += ["", "작업마다 out/<작업>/ 에 프롬프트(prompt.md), 모델의 답(answer-N.md), 결과 파일, 마지막 검사 내용(verify.txt)이 있습니다."]
    (OUT / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def self_check(tasks, args):
    """The pack itself: prompts and context resolve, every mutant still applies once, checkers exist."""
    problems = 0
    for t in tasks:
        issues = []
        if not (HERE / t["prompt"]).exists():
            issues.append(f"프롬프트 없음: {t['prompt']}")
        try:
            context_blocks(t, args)
        except (RuntimeError, OSError) as e:
            issues.append(f"문맥: {e}")
        for m in t["verify"].get("mutants", []):
            n = (ROOT / m["file"]).read_text(encoding="utf-8").count(m["find"])
            if n != 1:
                issues.append(f"변이 {m['id']}: {m['file']}에 찾는 문자열이 {n}번 있음 (1번이어야 함)")
        for a in t["verify"].get("cmd", []):
            if a.endswith(".py") and not (ROOT / a).exists():
                issues.append(f"검사 스크립트 없음: {a}")
        problems += len(issues)
        print(f"{t['id']}: " + ("문제 없음" if not issues else "\n    ".join(["문제 있음"] + issues)))
    print("작업 팩 점검: " + ("모두 정상" if not problems else f"문제 {problems}개"))
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description="Liquid Dock 로컬 LLM 작업 실행기 (tools/llm-tasks/README.md 참고)")
    ap.add_argument("--api", choices=["ollama", "openai"], default=os.environ.get("LLM_API", "ollama"),
                    help="ollama: Ollama 자체 API (기본). openai: OpenAI 호환 API (LM Studio, llama.cpp server, vLLM ...)")
    ap.add_argument("--base-url", default=os.environ.get("LLM_BASE_URL"),
                    help="기본값: ollama는 http://127.0.0.1:11434, openai는 http://127.0.0.1:1234/v1 (LM Studio)")
    ap.add_argument("--model", default=os.environ.get("LLM_MODEL", "qwen2.5-coder:14b"))
    ap.add_argument("--api-key", default=os.environ.get("LLM_API_KEY", ""))
    ap.add_argument("--num-ctx", type=int, default=16384, help="Ollama 문맥 크기(토큰). 프롬프트가 잘리지 않게 (기본 16384)")
    ap.add_argument("--tasks", default="all", help="쉼표로 구분한 작업 ID, 예: T1,T3 (기본: all)")
    ap.add_argument("--max-attempts", type=int, default=3)
    ap.add_argument("--temperature", type=float, default=0.2)
    ap.add_argument("--max-tokens", type=int, default=0, help="답의 최대 토큰 (0: 서버 기본값)")
    ap.add_argument("--timeout", type=int, default=1200, help="모델 응답 하나의 제한 시간(초)")
    ap.add_argument("--run-timeout", type=int, default=60, help="생성된 테스트 실행 제한 시간(초)")
    ap.add_argument("--cxx", default="auto", choices=["auto", "msvc", "g++", "clang++", "zig"])
    ap.add_argument("--diff-base", default="HEAD", help="T7이 검토할 변경의 기준 (기본 HEAD: 커밋 안 한 변경)")
    ap.add_argument("--no-mutants", action="store_true", help="변이 검사를 건너뜀 (빠르지만 테스트 품질 확인이 약해짐)")
    ap.add_argument("--apply", action="store_true", help="통과한 결과를 저장소의 제자리에 복사")
    ap.add_argument("--verify-only", action="store_true", help="모델 호출 없이 out의 결과만 다시 검사")
    ap.add_argument("--dry-run", action="store_true", help="프롬프트만 out에 저장")
    ap.add_argument("--allow-unsafe", action="store_true", help="위험 호출 확인을 끔 (권장하지 않음)")
    ap.add_argument("--list", action="store_true", help="작업 목록")
    ap.add_argument("--self-check", action="store_true", help="작업 팩 점검")
    args = ap.parse_args()
    if not args.base_url:
        args.base_url = "http://127.0.0.1:11434" if args.api == "ollama" else "http://127.0.0.1:1234/v1"

    tasks = load_tasks()
    if args.list:
        for t in tasks:
            print(f"{t['id']}  {t['title']}  →  {t['target']}  [{t['verify']['kind']}, {t.get('size', '')}]")
        return 0
    if args.tasks != "all":
        want = [x.strip().upper() for x in args.tasks.split(",") if x.strip()]
        unknown = sorted(set(want) - {t["id"] for t in tasks})
        if unknown:
            print("알 수 없는 작업:", ", ".join(unknown))
            return 2
        tasks = [t for t in tasks if t["id"] in want]
    if args.self_check:
        return self_check(tasks, args)

    tc = None
    if not args.dry_run and any(t["verify"]["kind"] == "cpp-test" for t in tasks):
        try:
            tc = Toolchain(args.cxx)
        except RuntimeError as e:
            print(e)
            return 2
    OUT.mkdir(parents=True, exist_ok=True)
    print(f"작업 {len(tasks)}개 · 모델 {args.model} ({args.api}, {args.base_url})" + (f" · 컴파일러 {tc.name}" if tc else ""))
    results = []
    for t in tasks:
        print(f"[{t['id']}] {t['title']}", flush=True)
        r = run_task(t, args, tc, lambda s: print(s, flush=True))
        results.append(r)
        print(f"  → {'준비' if args.dry_run else status(r, args)}: {r['note']}", flush=True)
    write_report(results, args, tc)
    if args.dry_run:
        print(f"\n프롬프트 {sum(not r.get('skipped') for r in results)}개를 저장했습니다: {OUT}")
        return 0
    counted = [r for r in results if not r.get("skipped") and not r.get("unchecked")]
    passed = sum(r["passed"] for r in counted)
    others = len(results) - len(counted)
    print(f"\n결과: {passed}/{len(counted)} 통과" + (f" (검사 없는 작업·건너뜀 {others}개 별도)" if others else "")
          + f" · 보고서 {OUT / 'report.md'}")
    return 0 if args.dry_run or passed == len(counted) else 1


if __name__ == "__main__":
    sys.exit(main())
