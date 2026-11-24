#!/usr/bin/env python3
"""두 빌드의 함수(COMDAT)별 기계어 비교.

  python tools\\refactor\\objdiff.py --a build\\refactor-scratch\\before --b build\\refactor-scratch\\after
  python tools\\refactor\\objdiff.py --a old\\x.obj old\\y.obj --b new\\x.obj new\\y.obj new\\z.obj
  python tools\\refactor\\objdiff.py --a A --b B --disasm 5        (다른 함수 5개까지 dumpbin /disasm 차이도 찍음)
  python tools\\refactor\\objdiff.py --a old\\LiquidDock.exe --b new\\LiquidDock.exe    (exe: PE 구역별 해시만)

두 트리를 따로 /c 로 컴파일해 .obj 폴더 둘을 만든 뒤 견준다. 이 도구가 컴파일도 한다(build.bat의 CF 그대로, build\\*.obj는 안 건드림):
  python tools\\refactor\\objdiff.py --compile HEAD~1 --compile-out build\\refactor-scratch\\obj-base     (git archive로 풀어서)
  python tools\\refactor\\objdiff.py --compile worktree --compile-out build\\refactor-scratch\\obj-now
  python tools\\refactor\\objdiff.py --a build\\refactor-scratch\\obj-base --b build\\refactor-scratch\\obj-now
  (--compile-files common.cpp ... 로 몇 파일만. --mp는 /MP: 빠르지만 CPU를 다 쓰니 다른 테스트가 돌 때는 쓰지 않는다)

어떻게 보나:
  - dumpbin 출력을 읽지 않고 COFF .obj(보통 꼴과 /bigobj 꼴)를 직접 읽는다: 구역 머리, 기호표, 문자열표, 재배치.
    /O2는 /Gy를 켜므로 함수마다 COMDAT 구역(.text$mn) 하나가 된다. 그 COMDAT 기호(구역 기호 다음의 첫 기호) 이름이 열쇠다.
    연관 COMDAT(.pdata, .xdata, 예외 처리 조각 .text$x: 선택 5)은 '부모 열쇠|구역 이름'으로 견준다(한 부모 아래 같은 이름이
    여럿이면 다중집합: .obj 안의 순서가 일정하지 않다).
  - 내용: 구역 특성(정렬 비트와 COMDAT 선택은 뺌: 선택은 static·익명 여부를 따르고, COMDAT 아닌 .data의 정렬은 같은 .obj의
    다른 변수를 따른다), 크기, 바이트, 재배치 목록(구역 안 위치, 종류, 대상). .obj의 재배치 자리 바이트는 링크 뒤 주소가
    아니라 더할 값(addend)이라 주소와 무관하므로 그대로 둔다(.obj 바이트 자체는 같은 소스를 두 번 컴파일해도 머리의 시각과
    .debug$S의 경로 때문에 다르다: 확인).
  - 재배치 대상: 이름으로 적되, 번역 단위마다 번호·위치가 달라지는 것은 내용으로 적는다. COMDAT 안이면 그 열쇠(+위치),
    연관 구역(.xdata, ?dtor$0@... 같은 조각)이면 '부모 열쇠 + 그 구역 바이트 해시', COMDAT 아닌 구역의 $SG·$LN 같은 지역
    꼬리표면 '그 위치부터 다음 기호까지의 바이트 해시', 구역 기호(.rdata+n)면 구역 이름만(더할 값은 0으로 지움: 느슨함).
  - 이름 정규화: 익명 namespace의 '?A0x' + 16진 8자리(번역 단위·경로마다 다른 해시: 확인)는 '?A0x*'로 바꾼다. 그래서 익명
    namespace·static 함수를 다른 .cpp로 옮겨도 짝지어진다. 같은 열쇠가 여러 개면(서로 다른 .cpp의 같은 이름 static 함수,
    여러 번역 단위에 복사된 inline·템플릿 함수, 문자열) 내용 해시의 집합으로 견준다:
      복사본 수만 다름 = 내용은 같고 개수만 다름(새 .cpp가 같은 헤더를 include하면 inline 복사본이 는다),
      복사본 변형 = 한쪽 복사본이 모두 다른 쪽에도 있고 다른 쪽에 변형이 더 있음(다른 번역 단위에서 인라인이 달리 됨).
    둘 다 판정에 넣지 않는다(정보).
  - 이름만 바뀐 것: 한쪽에만 있는 열쇠끼리 식별자(첫 '@' 앞)가 같고 '이름을 뺀 내용'이 같으면 짝짓고, 익명 namespace에서
    바깥 namespace로 나온 것('?A0x*@'만 빠진 이름)도 짝짓는다. 그 이름 바꿈을 A 쪽의 열쇠와 재배치 대상에 적용해 다시
    견준다(최대 5번). 짝지은 목록을 맨 앞에 찍는다(분할에서 내부 헤더로 옮겨 이름이 바뀐 것).
  - 함수 밖 데이터: COMDAT 아닌 구역(.data, .rdata, .CRT$XCU ...)의 기호는 기호마다 '위치부터 다음 기호까지, 마지막 재배치
    뒤의 0 바이트는 뺌'(채움 바이트는 배치를 따름)을 한 항목으로 견준다. .bss는 있는지만 본다. 꼬리표 없는 바이트,
    .debug$*, .chks64, .drectve는 보지 않는다.
  - 결과: 같음(그 가운데 다른 .obj로 옮겨진 것) / 복사본 수만 다름 / 복사본 변형 / 다름(그 가운데 재배치 대상 이름만 다름) /
    A에만 / B에만. 코드와 데이터의 다름·A에만·B에만이 모두 0이면 'OBJDIFF: 같음'(종료 코드 0). 코드만 같으면
    'OBJDIFF: 같음(코드) / 데이터 다름 N'(종료 코드 1: 상수 표, 전역 변수·정적 객체의 처음 값이 바뀐 것), 코드가 다르면
    'OBJDIFF: 다름[ / 데이터 다름 N]'(1). --data-loose면 데이터는 끝 줄에 수만 적고 종료 코드에 넣지 않는다(옛 기본).
    --disasm N은 다른 함수 N개의 dumpbin /disasm:nobytes 차이를 찍는다(주소 열을 빼고 이름을 정규화한 뒤 unified diff).
    함수는 들여쓰지 않은 '이름:' 줄에서만 나눈다. 함수 안의 '$LN20:' 같은 꼬리표 줄('$'로 시작)은 그 함수에 '$LN*:'로
    이어 붙인다(3-0 전에는 여기서 끊겨 큰 함수의 앞부분만 견줬다: settingswindow::Render 688줄 / 전체 3044줄). '$LN<n>'
    번호(번역 단위의 계수)는 피연산자에서도 '$LN*'로 지우고, 끝의 '  Summary' 구역은 어느 함수에도 넣지 않는다.
    diff 머리 줄에 두 쪽의 줄 수를 찍는다.

한계 (알고 쓸 것):
  - /O2 인라인: 함수를 다른 .cpp로 옮기면 옛 파일의 호출자가 그 함수를 더는 인라인하지 못해(또는 새 파일에서 새로
    인라인해) 호출자 쪽 기계어가 달라진다. 옮긴 함수가 '같음'이어도 호출자가 '다름'으로 나오면 이것부터 의심한다
    (--disasm으로 call이 생겼는지 본다). 반대로 인라인돼 사라진 static 함수는 '한쪽에만'으로 나온다. 몸이 바뀐 함수가
    인라인되는 곳의 호출자도 함께 '다름'이 된다(시험: Changed를 바꾸면 Caller도 다름, noinline이면 Changed만).
    같은 함수도 다른 번역 단위에서는 레지스터 배정·인라인이 달라질 수 있다(oled를 나눴을 때 옮긴 함수 8개가 이것으로 다름).
    그래서 파일 사이 이동은 '다름 0'이 아니라 '다름마다 --disasm으로 이유가 인라인·배정·이름뿐인지'를 사람이 본다.
  - 람다: MSVC의 람다 타입 이름은 '<lambda_N>@?k??둘러싼함수@@...' 꼴로 둘러싼 함수 안의 순번이다(시험으로 확인: 다른 함수에
    람다를 더하면 기존 람다 이름은 그대로, 같은 함수 안 앞에 더하면 뒤 람다가 <lambda_2>로 바뀜). 그래서 둘러싼 함수를
    옮기기만 하면 그대로다. 옛 꼴 <lambda_16진32자리>는 '<lambda_*>'로 바꾼다. --loose-lambdas면 순번도 지워 다중집합으로
    견준다(순서만 바뀐 람다가 같게 보일 수 있음). namespace 범위(함수 밖) 람다의 번호 매김은 시험하지 않았다(추측: 번역 단위 순번).
    std::function에 담긴 람다는 RTTI 이름 문자열(??_R0...)도 그 이름을 따른다.
  - 이름만 바뀐 것의 짝짓기는 짐작이다: 식별자와 내용이 같은 다른 함수(복사해 둔 함수)를 잘못 짝지을 수 있다(짝 목록을 읽는다).
    맹글된 이름의 역참조 숫자(U012 -> U01)가 바뀌는 중첩 이름(?dtor$0@?0???0State@...)은 이름이 아니라 내용으로 본다.
  - 정적 초기화 순서, 링크 순서(파일 이름 사전 순), 같은 이름 COMDAT 가운데 링커가 무엇을 고르는지, /OPT:REF로 버려지는
    것은 안 본다. .obj 안 COMDAT의 차례(구역 번호)와 .drectve(링커 지시: /DEFAULTLIB, /INCLUDE, /alternatename ...)도
    보지 않는다. 그래서 'OBJDIFF: 같음'이어도 exe 바이트는 다를 수 있다(함수 배치·주소: 링커가 .obj 안 차례를 따라
    놓는다고 보는 것은 추측, 시험하지 않음; .drectve가 다르면 링크되는 라이브러리·기호가 달라짐).
    exe가 같은지는 gate.py의 해시로 보고, 다르면 이 도구로 원인을 본다.
  - exe 두 개를 주면 PDB가 없어(/Zi 없음, /Brepro) 함수 경계를 모른다: PE 구역별 크기와 해시만 견준다.
  - 재배치 종류는 AMD64 기준(x64 빌드만 씀). /GL(LTCG) .obj는 기계어가 없어 읽지 못한다(build.bat은 /GL을 안 씀).
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IMAGE_SCN_CNT_CODE = 0x20
IMAGE_SCN_CNT_UNINIT = 0x80
IMAGE_SCN_LNK_INFO = 0x200
IMAGE_SCN_LNK_REMOVE = 0x800
IMAGE_SCN_LNK_COMDAT = 0x1000
IMAGE_SCN_LNK_NRELOC_OVFL = 0x01000000
SKIP_SECTIONS = re.compile(r"^(\.debug\$|\.chks64$|\.drectve$|\.llvm_addrsig$)")
RELOC_SIZE = {1: 8, 2: 4, 3: 4, 4: 4, 5: 4, 6: 4, 7: 4, 8: 4, 9: 4, 0xA: 2, 0xB: 4, 0xE: 4, 0xF: 4, 0x10: 4}
BIGOBJ_CLSID = bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")


def norm_name(name, loose_lambdas=False):
    n = re.sub(r"\?A0x[0-9a-fA-F]{8}", "?A0x*", name)
    n = re.sub(r"<lambda_[0-9a-f]{32}>", "<lambda_*>", n)
    if loose_lambdas:
        n = re.sub(r"<lambda_\d+>", "<lambda_*>", n)
    return n


def is_local_label(name):
    return bool(re.match(r"^(\$SG\d+|\$LN\d+|\$T\d+|\$S\d+|\$M\d+|\$L\d+|\$unwind\$|\$pdata\$|\$chain\$|\$ip2state\$|\$cppxdata\$|\$stateUnwindMap\$|\$tryMap\$|\$handlerMap\$)", name))


class Obj:
    def __init__(self, path: Path, loose_lambdas=False):
        self.path = path
        data = path.read_bytes()
        self.data = data
        sig1, sig2 = struct.unpack_from("<HH", data, 0)
        if sig1 == 0 and sig2 == 0xFFFF:
            ver, machine = struct.unpack_from("<HH", data, 4)
            if data[12:28] != BIGOBJ_CLSID:
                raise ValueError(f"{path}: COFF .obj가 아님 (import 객체 또는 /GL 객체?)")
            nsec, symptr, nsym = struct.unpack_from("<III", data, 44)
            hdr, symsize, bigobj = 56, 20, True
        else:
            machine, nsec, _, symptr, nsym, opt, _ = struct.unpack_from("<HHIIIHH", data, 0)
            hdr, symsize, bigobj = 20 + opt, 18, False
        self.machine = machine
        strtab = symptr + nsym * symsize

        def string_at(off):
            end = data.index(b"\0", strtab + off)
            return data[strtab + off:end].decode("utf-8", "replace")

        self.sections = []
        for i in range(nsec):
            o = hdr + 40 * i
            name8, vsize, vaddr, rawsize, rawptr, relptr, _, nrel, _, ch = struct.unpack_from("<8sIIIIIIHHI", data, o)
            name = name8.rstrip(b"\0").decode("utf-8", "replace")
            if name.startswith("/"):
                name = string_at(int(name[1:]))
            if ch & IMAGE_SCN_LNK_NRELOC_OVFL and nrel == 0xFFFF:
                nrel = struct.unpack_from("<I", data, relptr)[0]
                relptr += 10
                nrel -= 1
            raw = b"" if (ch & IMAGE_SCN_CNT_UNINIT or rawptr == 0) else data[rawptr:rawptr + rawsize]
            relocs = [struct.unpack_from("<IIH", data, relptr + 10 * k) for k in range(nrel)]
            self.sections.append({"name": name, "ch": ch, "size": rawsize, "raw": raw, "relocs": relocs,
                                  "sel": None, "assoc": None, "comdat_sym": None, "syms": []})
        self.syms = [None] * nsym
        i = 0
        while i < nsym:
            o = symptr + i * symsize
            if bigobj:
                name8, value, secnum, typ, scl, naux = struct.unpack_from("<8sIiHBB", data, o)
            else:
                name8, value, secnum, typ, scl, naux = struct.unpack_from("<8sIhHBB", data, o)
            if name8[:4] == b"\0\0\0\0":
                name = string_at(struct.unpack_from("<I", name8, 4)[0])
            else:
                name = name8.rstrip(b"\0").decode("utf-8", "replace")
            sym = {"name": name, "value": value, "sec": secnum, "scl": scl, "is_section": False}
            if 0 < secnum <= nsec and scl == 3 and naux == 1 and value == 0 and name == self.sections[secnum - 1]["name"]:
                # section definition: its aux record carries the COMDAT selection and the associated section
                ao = o + symsize
                length, nr, nl, chk, num, sel = struct.unpack_from("<IHHIHB", data, ao)
                if bigobj:
                    num |= struct.unpack_from("<H", data, ao + 14)[0] << 16
                s = self.sections[secnum - 1]
                if s["ch"] & IMAGE_SCN_LNK_COMDAT:
                    s["sel"], s["assoc"] = sel, (num if sel == 5 else None)
                sym["is_section"] = True
            elif 0 < secnum <= nsec:
                s = self.sections[secnum - 1]
                if s["ch"] & IMAGE_SCN_LNK_COMDAT and s["sel"] not in (None, 5) and s["comdat_sym"] is None:
                    s["comdat_sym"] = name
                s["syms"].append((value, name, scl))
            self.syms[i] = sym
            i += 1 + naux
        for s in self.sections:
            s["syms"].sort()
        self.loose_lambdas = loose_lambdas
        self._keys = {}

    # key of section number n (1-based)
    def key_of(self, n, depth=0):
        if n in self._keys:
            return self._keys[n]
        s = self.sections[n - 1]
        if not s["ch"] & IMAGE_SCN_LNK_COMDAT:
            k = None
        elif s["sel"] == 5 and s["assoc"] and depth < 8:
            # associative (.pdata, .xdata, .text$x funclets): the parent's key + the section name; several of one name
            # under one parent are compared as a multiset (their order in the .obj is not stable)
            parent = self.key_of(s["assoc"], depth + 1) or f"<non-COMDAT {self.sections[s['assoc'] - 1]['name']}>"
            k = f"{parent}|{s['name']}"
        elif s["comdat_sym"]:
            k = norm_name(s["comdat_sym"], self.loose_lambdas)
        else:
            k = f"<COMDAT without symbol {s['name']}>"
        self._keys[n] = k
        return k

    def extent(self, secnum, value):
        """Bytes of the symbol at value: up to the next symbol of the section, less trailing zero bytes after the last
        relocation (padding before the next object changes with the layout). Uninitialized data: (value, value), so
        only that it exists is compared."""
        import bisect
        s = self.sections[secnum - 1]
        if not s["raw"]:
            return value, value
        if "_vals" not in s:
            s["_vals"] = sorted({v for v, _, _ in s["syms"]})
            pairs = sorted((va, va + RELOC_SIZE.get(t, 4)) for va, _, t in s["relocs"])
            s["_relstart"] = [p[0] for p in pairs]
            s["_relend"] = [p[1] for p in pairs]
        i = bisect.bisect_right(s["_vals"], value)
        end = s["_vals"][i] if i < len(s["_vals"]) else len(s["raw"])
        k = bisect.bisect_left(s["_relstart"], value)
        j = bisect.bisect_left(s["_relstart"], end)          # relocations that start inside value..end
        last = max(s["_relend"][k:j]) if j > k else value
        while end > max(value, last) and s["raw"][end - 1] == 0:
            end -= 1
        return value, end

    def target(self, symidx, here_sec):
        """(description, loose) of a relocation's target, independent of addresses and of per-TU numbering."""
        sym = self.syms[symidx] if symidx < len(self.syms) else None
        if sym is None:
            return "<bad symbol index>", False
        name, sec = sym["name"], sym["sec"]
        if sec <= 0:                                   # undefined here (another .obj, a library) or absolute
            return norm_name(name, self.loose_lambdas), False
        value = sym["value"]
        if sec == here_sec:
            return f"self+{value}", False
        k = self.key_of(sec)
        t = self.sections[sec - 1]
        if k and t["sel"] == 5:
            # into an associative section (.xdata, a .text$x funclet such as ?dtor$0@...): its parent and its bytes,
            # whatever the symbol is called (funclet names spell their parent's name)
            return f"[{k}:{hashlib.sha1(t['raw']).hexdigest()[:12]}]+{value}", False
        if k:
            # into a COMDAT: its key (the same whether the reference is by its name, its section or a label)
            return (k if value == 0 else f"[{k}]+{value}"), False
        if sym["is_section"]:
            return f"sec:{t['name']}", True            # the addend (in the bytes) is an offset into it: zeroed by the caller
        if not (is_local_label(name) or sym["scl"] == 6):
            return norm_name(name, self.loose_lambdas), False
        a, b = self.extent(sec, value)
        return f"data:{t['name']}:{hashlib.sha1(t['raw'][a:b]).hexdigest()[:16]}:{b - a}", False

    def items(self):
        """{'code'|'data': [(key, base hash, relocations, size)]} for this object."""
        out = {"code": [], "data": []}
        for n, s in enumerate(self.sections, 1):
            if SKIP_SECTIONS.match(s["name"]) or s["ch"] & IMAGE_SCN_LNK_REMOVE and s["ch"] & IMAGE_SCN_LNK_INFO:
                continue
            kind = "code" if s["ch"] & IMAGE_SCN_CNT_CODE else "data"
            k = self.key_of(n)
            if k is not None:
                out[kind].append((k, *self.content(n, 0, len(s["raw"]) if s["raw"] else s["size"]), s["size"]))
            else:
                for value, name, scl in s["syms"]:
                    if is_local_label(name) or name.startswith("."):
                        continue
                    a, b = self.extent(n, value)
                    out[kind].append((norm_name(name, self.loose_lambdas), *self.content(n, a, b), b - a))
        return out

    def content(self, n, a, b):
        """(hash of flags, size and bytes, ((offset, type, target), ...)) of bytes a..b of section n."""
        s = self.sections[n - 1]
        raw = bytearray(s["raw"][a:b]) if s["raw"] else bytearray()
        rel = []
        for va, si, typ in s["relocs"]:
            if not (a <= va < b):
                continue
            desc, loose = self.target(si, n)
            if loose:
                w = RELOC_SIZE.get(typ, 4)
                raw[va - a:va - a + w] = b"\0" * w
            rel.append((va - a, typ, desc))
        h = hashlib.sha256()
        # not the COMDAT selection: it follows the linkage (static or anonymous namespace or not), not the code
        # nor the alignment bits: a non-COMDAT .data/.bss takes the largest alignment of what else is in that .obj
        h.update(f"{s['ch'] & ~(IMAGE_SCN_LNK_NRELOC_OVFL | 0x00F00000):x}:{b - a}\n".encode())
        h.update(bytes(raw))
        return h.hexdigest(), tuple(rel)


def map_key(k, ren):
    parent, sep, rest = k.partition("|")
    return ren.get(parent, parent) + sep + rest


def map_desc(d, ren):
    if d in ren:
        return ren[d]
    m = re.match(r"^\[(.*?)(:[0-9a-f]{12})?\]\+(\d+)$", d)
    if m:
        return f"[{map_key(m.group(1), ren)}{m.group(2) or ''}]+{m.group(3)}"
    return d


def digest(base, rels, ren=None, namefree=False):
    h = hashlib.sha256(base.encode())
    for off, typ, d in rels:
        if namefree and not re.match(r"^(self\+|data:|sec:)", d):
            d = "*"
        elif ren:
            d = map_desc(d, ren)
        h.update(f"\n{off}:{typ}:{d}".encode())
    return h.hexdigest()


def collect(paths, loose_lambdas):
    objs = []
    for p in paths:
        p = Path(p)
        if p.is_dir():
            objs += sorted(p.glob("*.obj"))
        elif p.exists():
            objs.append(p)
        else:
            raise SystemExit(f"없음: {p}")
    res = defaultdict(list)
    for o in objs:
        ob = Obj(o, loose_lambdas)
        for kind, lst in ob.items().items():
            for k, base, rels, size in lst:
                res[k].append((base, rels, size, o.name, kind))
    return objs, res


def compare_once(a, b, ren):
    """A's keys and relocation targets go through ren (the renames found so far) before the comparison."""
    am = defaultdict(list)
    for k, v in a.items():
        am[map_key(k, ren)] += v
    out = {"same": [], "copies": [], "variants": [], "diff": [], "only_a": [], "only_b": [], "moved": [], "names_only": []}
    for k in sorted(set(am) | set(b)):
        A, B = am.get(k, []), b.get(k, [])
        if not B:
            out["only_a"].append(k)
            continue
        if not A:
            out["only_b"].append(k)
            continue
        da = Counter(digest(x[0], x[1], ren) for x in A)
        db = Counter(digest(x[0], x[1]) for x in B)
        if set(da) != set(db) and (set(da) < set(db) or set(db) < set(da)):
            out["variants"].append(k)      # one side's copies are all among the other's: an inline copy compiled
            #                                 differently in another translation unit (inlining there)
        elif set(da) != set(db):
            out["diff"].append(k)
            if {digest(x[0], x[1], namefree=True) for x in A} == {digest(x[0], x[1], namefree=True) for x in B}:
                out["names_only"].append(k)
        elif da != db:
            out["copies"].append(k)
        else:
            out["same"].append(k)
            if {x[3] for x in A} != {x[3] for x in B}:
                out["moved"].append(k)
    return out, am


def ident(k):
    m = re.match(r"^\?*([^@|]+)", k)
    return m.group(1) if m else k


def find_renames(out, am, b):
    """Pairs a key only in A with one only in B when both have the same identifier (the name before the first '@')
    and the same name-free content, one to one. Returns {mapped A key: B key} (top-level keys only)."""
    def nf(items):
        return frozenset(digest(x[0], x[1], namefree=True) for x in items)
    cand_b, cand_a = defaultdict(list), defaultdict(list)
    for kb in out["only_b"]:
        if "|" not in kb:
            cand_b[(ident(kb), nf(b[kb]))].append(kb)
    for ka in out["only_a"]:
        if "|" not in ka:
            cand_a[(ident(ka), nf(am[ka]))].append(ka)
    out_map = {cand_a[c][0]: cand_b[c][0] for c in cand_a if len(cand_a[c]) == 1 and len(cand_b.get(c, [])) == 1}
    # taken out of an anonymous namespace into the one around it (a split's internal header): the same name without
    # '?A0x*@', even when the code differs (it is then compared under the new name and shows as 'different')
    only_b = set(out["only_b"])
    for ka in out["only_a"]:
        kb = ka.replace("?A0x*@", "")
        if "|" not in ka and ka not in out_map and kb != ka and kb in only_b and kb not in out_map.values():
            out_map[ka] = kb
    return out_map


def compare(a, b, renames=True):
    """compare_once; then up to 4 rounds of: pair renamed keys (same content under a new name), compare again with
    those names mapped (in keys and in relocation targets)."""
    ren = {}
    for _ in range(5):
        out, am = compare_once(a, b, ren)
        if not renames:
            break
        new = find_renames(out, am, b)
        inverse = {}
        for orig, mapped in ren.items():
            inverse[mapped] = orig
        new = {inverse.get(k, k): v for k, v in new.items()}
        new = {k: v for k, v in new.items() if ren.get(k) != v}
        if not new:
            break
        ren.update(new)
    out["renamed"] = sorted(ren.items())
    out["am"] = am
    return out


# ------------------------------------------------------------ dumpbin, for a look at what differs
_dis_cache = {}


LOCAL_LN = re.compile(r"\$LN\d+")


def split_disasm(text):
    """{function name: [instruction lines]} of dumpbin /disasm output. A function starts at an unindented 'name:' or
    'name (undecorated):' line. An unindented line that starts with '$' ('$LN20:', a jump-table target or a label the
    compiler numbered inside the function) is not a new function: it stays in the one it is in, as a '$LN*:' line
    (the numbers are a per-translation-unit counter, so '$LN<n>' in operands is masked the same way). The '  Summary'
    block at the end belongs to no function."""
    funcs, cur = {}, None
    for line in text.split("\n"):
        if line.startswith("$"):
            if cur is not None:
                funcs[cur].append(LOCAL_LN.sub("$LN*", line.rstrip()))
            continue
        if line.rstrip() == "  Summary":
            cur = None
            continue
        m = re.match(r"^(\S.*?)(?: \(.*\))?:$", line)
        if m and not line.startswith(" "):
            cur = norm_name(m.group(1))
            funcs.setdefault(cur, [])
            continue
        if cur and line.startswith("  "):
            # drop the address column and absolute addresses so only the instructions are compared
            funcs[cur].append(LOCAL_LN.sub("$LN*", norm_name(re.sub(r"^\s*[0-9A-F]{16}: ", "", line).rstrip())))
    return funcs


def disasm(obj: Path):
    if obj in _dis_cache:
        return _dis_cache[obj]
    cmd = f'cmd /d /s /c "call .\\tests\\vsenv.bat >nul 2>nul && dumpbin /nologo /disasm:nobytes "{obj}""'
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True)
    funcs = split_disasm(r.stdout.decode("utf-8", "replace").replace("\r", ""))
    _dis_cache[obj] = funcs
    return funcs


def show_disasm(key_a, key, A, B, dirs_a, dirs_b, limit_lines=60):
    import difflib
    def where(items, dirs):
        for x in items:
            for d in dirs:
                d = Path(d)
                p = d / x[3] if d.is_dir() else d if d.name == x[3] else None
                if p and p.exists():
                    return p
        return None
    oa, ob = where(A, dirs_a), where(B, dirs_b)
    if not (oa and ob):
        print("    (dumpbin: .obj를 찾지 못함)")
        return
    fa, fb = disasm(oa).get(key_a), disasm(ob).get(key)
    if fa is None or fb is None:
        print("    (dumpbin 출력에서 이 이름을 찾지 못함: 데이터이거나 이름 꼴이 다름)")
        return
    d = list(difflib.unified_diff(fa, fb, f"A/{oa.name} ({len(fa)}줄)", f"B/{ob.name} ({len(fb)}줄)", lineterm="", n=2))
    if not d:
        print("    (dumpbin 글자는 같음: 재배치 대상이나 연관 구역(.pdata/.xdata/.text$x)만 다름)")
    for line in d[:limit_lines]:
        print("    " + line)
    if len(d) > limit_lines:
        print(f"    ... {len(d) - limit_lines}줄 더")


def pe_sections(path: Path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    _, nsec, _, _, _, opt, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
    out = {}
    o = pe + 24 + opt
    for i in range(nsec):
        name8, vsize, vaddr, rawsize, rawptr = struct.unpack_from("<8sIIII", data, o + 40 * i)
        out[name8.rstrip(b"\0").decode()] = (rawsize, hashlib.sha256(data[rawptr:rawptr + rawsize]).hexdigest())
    return out


def build_cf():
    bat = (ROOT / "build.bat").read_text(encoding="utf-8", errors="replace")
    defs = re.search(r'^set "DEFS=([^"]*)"', bat, re.M)
    cf = re.search(r'^set "CF=([^"]*)"', bat, re.M)
    if not (defs and cf):
        raise SystemExit("build.bat의 CF/DEFS 줄을 읽지 못했습니다")
    return cf.group(1).replace("%DEFS%", defs.group(1))


def compile_tree(rev, out: Path, files, mp):
    """Compiles src\\*.cpp (or the named files) of a revision (git archive into build\\refactor-scratch\\tree-<rev>) or of
    the work tree with build.bat's CF into out. Never into build\\ itself or build\\tests."""
    import tarfile, io
    out = out.resolve()
    for bad in (ROOT / "build", ROOT / "build" / "tests", ROOT / "build" / "sensors", ROOT / "build" / "setup"):
        if out == bad.resolve():
            raise SystemExit(f"{out}에는 컴파일하지 않습니다 (build.bat·테스트의 .obj 자리). build\\refactor-scratch\\... 를 쓰세요")
    if rev:
        sha = subprocess.run(["git", "rev-parse", "--verify", rev + "^{commit}"], cwd=ROOT, capture_output=True)
        if sha.returncode:
            raise SystemExit(f"리비전을 모릅니다: {rev}")
        tree = ROOT / "build" / "refactor-scratch" / ("tree-" + sha.stdout.decode().strip()[:12])   # by commit, not by name
        if not tree.exists():
            data = subprocess.run(["git", "archive", "--format=tar", rev, "src"], cwd=ROOT, capture_output=True)
            if data.returncode:
                raise SystemExit(f"git archive {rev} 실패: {data.stderr.decode('utf-8', 'replace')}")
            tree.mkdir(parents=True)
            with tarfile.open(fileobj=io.BytesIO(data.stdout)) as tf:
                tf.extractall(tree, filter="data")
    else:
        tree = ROOT
    srcs = [f"src\\{f}" for f in files] if files else ["src\\*.cpp"]
    out.mkdir(parents=True, exist_ok=True)
    cmd = (f'cmd /d /s /c "call "{ROOT}\\tests\\vsenv.bat" >nul 2>nul && cl {build_cf()}{" /MP" if mp else ""} /c '
           f'/Fo"{out}\\\\" {" ".join(srcs)}"')
    t0 = time.time()
    r = subprocess.run(cmd, cwd=tree, capture_output=True)
    text = r.stdout.decode("cp949", "replace") + r.stderr.decode("cp949", "replace")
    warn = [l for l in text.splitlines() if re.search(r"(warning|error) (C|D)\d+", l)]
    print(f"compile {rev or '작업 트리'} ({tree}) -> {out}: exit {r.returncode}, warning/error {len(warn)}, "
          f"{len(list(out.glob('*.obj')))}개 .obj, {time.time() - t0:.0f} s")
    for l in warn[:20]:
        print("  " + l)
    return 0 if r.returncode == 0 and not warn else 1


def main():
    ap = argparse.ArgumentParser(description="두 빌드의 .obj를 함수(COMDAT)별로 견준다 (tools/refactor/README.md)")
    ap.add_argument("--a", nargs="+", help="옛 쪽: .obj 폴더(들) 또는 .obj 파일들 (또는 exe 하나)")
    ap.add_argument("--b", nargs="+", help="새 쪽")
    ap.add_argument("--compile", metavar="REV|worktree", help="비교하지 않고, 그 리비전(또는 'worktree')의 src\\*.cpp를 "
                    "build.bat의 CF로 --compile-out에 /c 컴파일한다")
    ap.add_argument("--compile-out", type=Path, help="--compile의 .obj 폴더 (build\\refactor-scratch\\ 아래)")
    ap.add_argument("--compile-files", nargs="+", help="--compile에서 src\\ 아래 이 파일만 (예: common.cpp)")
    ap.add_argument("--mp", action="store_true", help="--compile에 /MP (빨라지지만 CPU를 다 씀: 다른 테스트가 돌 때는 쓰지 말 것)")
    ap.add_argument("--loose-lambdas", action="store_true", help="람다 순번(<lambda_N>)도 지우고 다중집합으로 견줌")
    ap.add_argument("--data-loose", action="store_true", help="데이터(함수 밖 기호)의 다름·A에만·B에만을 종료 코드에 넣지 않음 "
                    "(끝 줄에는 수를 적음)")
    ap.add_argument("--data-strict", action="store_true", help="옛 옵션: 지금은 기본이 이것과 같다(아무 일 안 함)")
    ap.add_argument("--disasm", type=int, default=0, help="다른 함수 N개까지 dumpbin /disasm 차이를 찍음 (vsenv 필요)")
    ap.add_argument("--limit", type=int, default=60, help="목록마다 찍을 이름 수")
    ap.add_argument("--no-renames", action="store_true", help="이름만 바뀐 것을 짝짓지 않음")
    ap.add_argument("-v", "--verbose", action="store_true", help="'복사본 수만 다름' 목록도 찍음")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace"); sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    if args.compile:
        if not args.compile_out:
            ap.error("--compile에는 --compile-out이 필요합니다")
        return compile_tree(None if args.compile == "worktree" else args.compile, args.compile_out, args.compile_files, args.mp)
    if not (args.a and args.b):
        ap.error("--a와 --b가 필요합니다 (또는 --compile)")
    if len(args.a) == 1 and len(args.b) == 1 and args.a[0].lower().endswith(".exe") and args.b[0].lower().endswith(".exe"):
        sa, sb = pe_sections(Path(args.a[0])), pe_sections(Path(args.b[0]))
        print("exe 비교 (PE 구역별 크기·해시; 함수 경계는 모름)")
        same = True
        for n in sorted(set(sa) | set(sb)):
            x, y = sa.get(n), sb.get(n)
            st = "같음" if x == y else "다름"
            same &= x == y
            print(f"  {n:<10} {st}  A {x[0] if x else '-'} / B {y[0] if y else '-'} 바이트")
        print("EXE: " + ("같음" if same else "다름"))
        return 0 if same else 1

    t0 = time.time()
    objs_a, A = collect(args.a, args.loose_lambdas)
    objs_b, B = collect(args.b, args.loose_lambdas)
    t1 = time.time()
    print(f"objdiff: A {len(objs_a)}개 .obj ({sum(p.stat().st_size for p in objs_a) // 1024} KB), "
          f"B {len(objs_b)}개 .obj ({sum(p.stat().st_size for p in objs_b) // 1024} KB), 읽기 {t1 - t0:.1f} s")
    code_bad = data_bad = 0
    r = compare(A, B, renames=not args.no_renames)
    am, ren = r["am"], dict(r["renamed"])
    inv = {v: k for k, v in ren.items()}

    def kind_of(k):
        items = B.get(k) or am.get(k) or []
        return items[0][4] if items else "data"

    def info(items):
        return f"{','.join(sorted({x[3] for x in items})) or '-'} {sorted({x[2] for x in items}) or ''}"
    if r["renamed"]:
        print(f"\n이름만 바뀐 것으로 짝지은 열쇠 {len(r['renamed'])}개 (식별자와 이름 뺀 내용이 같음; 재배치 대상도 이 이름으로 바꿔 견줌):")
        for ka, kb in r["renamed"][: args.limit]:
            print(f"    {ka}\n      -> {kb}")
        if len(r["renamed"]) > args.limit:
            print(f"    ... {len(r['renamed']) - args.limit}개 더")
    for kind, title in (("code", "함수(코드 COMDAT·코드 기호)"), ("data", "데이터(COMDAT·기호; .pdata/.xdata 포함)")):
        part = {lab: [k for k in lst if kind_of(k) == kind] for lab, lst in r.items() if isinstance(lst, list) and lab != "renamed"}
        na = sum(len(v) for k, v in am.items() if kind_of(k) == kind)
        nb = sum(len(v) for k, v in B.items() if kind_of(k) == kind)
        print(f"\n{title}: A {na}개, B {nb}개")
        print(f"  같음 {len(part['same'])} (그 가운데 다른 .obj로 옮겨진 것 {len(part['moved'])}), 복사본 수만 다름 {len(part['copies'])}, "
              f"복사본 변형 {len(part['variants'])}, 다름 {len(part['diff'])} (그 가운데 재배치 대상 이름만 다름 {len(part['names_only'])}), "
              f"A에만 {len(part['only_a'])}, B에만 {len(part['only_b'])}")
        for label, lst in (("다름", part["diff"]), ("A에만", part["only_a"]), ("B에만", part["only_b"]),
                           ("복사본 변형(한쪽 복사본이 다른 쪽에도 모두 있음)", part["variants"]),
                           ("옮겨짐(같음)", part["moved"]), ("복사본 수만 다름", part["copies"])):
            if not lst or (label in ("복사본 수만 다름", "옮겨짐(같음)") and not args.verbose):
                continue
            print(f"  {label}:")
            for k in lst[: args.limit]:
                mark = " (재배치 대상 이름만 다름)" if k in part["names_only"] else ""
                print(f"    {k}  [A {info(am.get(k, []))} | B {info(B.get(k, []))}]{mark}")
                if kind == "code" and label == "다름" and args.disasm > 0:
                    args.disasm -= 1
                    show_disasm(map_key(k, inv), k, am[k], B[k], args.a, args.b)
            if len(lst) > args.limit:
                print(f"    ... {len(lst) - args.limit}개 더")
        bad = len(part["diff"]) + len(part["only_a"]) + len(part["only_b"])
        if kind == "code":
            code_bad = bad
        else:
            data_bad = bad
    print(f"\n걸린 시간 {time.time() - t0:.1f} s")
    # data (constant tables, the first values of globals and static objects) is judged too unless --data-loose
    fail = code_bad > 0 or (data_bad > 0 and not args.data_loose)
    verdict = "다름" if code_bad else "같음(코드)" if data_bad else "같음"
    if data_bad:
        verdict += f" / 데이터 다름 {data_bad}" + (" (--data-loose: 판정에 안 넣음)" if args.data_loose else "")
    print("OBJDIFF: " + verdict)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
