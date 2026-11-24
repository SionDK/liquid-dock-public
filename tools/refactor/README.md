# 리팩토링 검사 도구 (tools\refactor)

'동작·UI는 그대로'인 리팩토링을 단계마다 확인하는 도구 다섯입니다. Python 3 표준 라이브러리만 씁니다. MSVC가 필요한 일
(컴파일, dumpbin)은 `tests\vsenv.bat`을 불러 씁니다. 만드는 파일은 모두 `build\refactor-scratch\` 아래에 둡니다
(`build\*.obj`와 `build\tests\`는 덮지 않습니다). 자세한 사용법과 한계는 각 파일 머리의 설명에 있습니다.

**이 문서의 예는 이 도구를 처음 쓴 mod.36 리팩토링(2026-11-20 ~ 11-21) 때의 것입니다.** 단계 이름(0-3, 2b, 6-3 ...)과
기준 폴더 이름(`baseline-*`)은 그 리팩토링 때의 예이고, 기준 폴더는 git에 넣지 않는 `userResult\` 아래에 두므로 저장소에는 없습니다.
기준은 0-3 뒤 `baseline-0-3` -> Windows 26H2 누적 업데이트 뒤 `baseline-0-3-os`(화소 HASH 510개가 달라짐) ->
시작 버튼 변경(6-3) 뒤 `baseline-6-3`으로 바뀌었고, 마지막 관문의 기준은 `baseline-6-3`이었습니다. 새 clone에서는
아래 '다른 PC에서 기준을 새로 잡기'대로 그 PC의 지금 트리에서 기준을 새로 잡습니다.

| 도구 | 보는 것 | 못 보는 것 |
|---|---|---|
| `gate.py` | build.bat → run_regressions --logged → run_dock_interactions를 차례로 돌리고 기준과 대조합니다. 경고·오류 0, 'is in use - skipped' 없음, 도우미·설치 파일을 실제로 컴파일했는지, 세 exe의 sha256(도우미가 다르면 FAIL), 짝 없는 옛 `build\*.obj`, 테스트별 결과 줄과 검사 수(여섯 가지 옛 꼴과 '이름: ALL PASSED (N checks, 0 failures)'를 함께 셈), `^PASS` 줄 다중집합(견주기 전에 양쪽에서 start_menu_run_test 판정 문자열의 Windows 빌드와 날짜 수를 자리표로 바꿈: `"failed 26100.4652 155550"` -> `"failed <build> <day>"`. 날짜 수는 UTC 자정, 곧 09:00 KST마다 바뀜), FAIL·FAILED:·도구 경고·오류 코드 줄(`warning [A-Z]+N`: C, D9xxx, LNK, RC ...), 끝 줄과 종료 코드, 매트릭스 cases·checks·failed_checks, 특성 테스트의 `HASH: <이름> <16진수>` 줄(두 로그에서 뽑아 기준의 `hash-lines.txt`와 이름별로: 값 다름·사라짐·한 실행에 두 번은 FAIL, 새 이름은 INFO, 어느 쪽에라도 `UNSTABLE: <이름>`이면 뺌). 건너뛰지 않는 구역의 기준 파일이 없으면 돌리지 않고 종료 코드 2, `--out`이 기준 폴더와 같아도 2. `--dev-compile`은 `#ifdef LD_DEV` 블록을, `run_tasks.py --self-check`는 LLM 작업 팩을 봅니다(판정에는 안 넣음). | 실기기 동작(실기기 테스트는 돌리지 않음), 검사 수가 같은데 검사 내용이 약해진 것, 이름 없는 'ALL PASSED (0 failures)' 두 줄의 주인(tray_host, fluent_flyout)은 전체 로그에서는 나오는 순서로만 가림(`--rerun-failed`의 exe별 로그에서는 그 테스트에 붙임) |
| `movecheck.py` | 두 리비전(또는 작업 트리) 사이에 사라진 줄 묶음이 다른 곳에 글자 그대로 나타났는지. 옮기지 않고 바뀐 줄, 주석, '접착', '꼴만 바뀜', 순서가 뒤집힌 묶음, 같은 파일 안에서 옮긴 묶음(옛·새 줄 범위), 짧은 조각을 나눠 찍습니다. 접착은 include·#pragma once, include guard(`#ifndef X`/값 없는 `#define X`/`#endif`뿐), namespace 여닫기, 혼자 선 괄호, 접근 지정자, using, 전방 선언, extern, 들여쓰지 않은 선언 한 줄(`return`·`throw`·`delete`·`else` 들로 시작하면 문장)입니다. 들여쓴 선언 꼴 줄(함수 안 `std::lock_guard<...> lock(m);`)은 반대쪽에 같은 줄이 있을 때만 접착이고, 짝 없는 접착은 `?`로 찍습니다. 꼴만 바뀜은 파일 범위 줄의 앞 static/inline, 함수 머리 줄의 기본 인자 `= 값`(한쪽에만 있거나 같은 값; `==`·`>=`·`for (int i = 0` 아님), 리터럴 밖의 끝 주석과 공백만 지운 짝입니다. `--strict`는 꼴만 바뀜·짝 없는 접착·순서 뒤집힘·같은 파일 안 이동·짧은 조각도 실패. | 뜻: 다른 namespace·가짜 아래로 옮겨진 것, #ifdef 안팎, 정적 초기화 순서. 한 줄짜리 조각은 위치를 보장 못 함(판정 줄의 '짧은 조각 n'을 보고 -v로 읽음). 선언의 시그니처가 바뀐 것은 짝 없는 접착 두 줄로 보일 수 있음 |
| `objdiff.py` | 두 .obj 폴더의 함수(COMDAT)별 기계어와 재배치. 파일이 달라도 이름으로 짝짓고, 익명 namespace 해시를 지우고, 이름만 바뀐 것(익명 namespace에서 나온 것 포함)을 짝짓습니다. `--compile REV|worktree`로 비교할 .obj를 scratch에 만듭니다. 함수 밖 데이터(상수 표, 전역·정적 객체의 처음 값)도 견주어, 코드만 같으면 끝 줄 `OBJDIFF: 같음(코드) / 데이터 다름 N`과 종료 코드 1입니다(`--data-loose`면 수만 적고 0). | /O2 인라인·레지스터 배정이 번역 단위마다 달라 옮긴 함수와 그 호출자가 '다름'으로 나올 수 있음(--disasm으로 사람이 봄). 링크 순서, 링커가 고르는 COMDAT, exe 안의 함수 경계. .obj 안 COMDAT의 차례와 `.drectve`(링커 지시)도 보지 않아, 'OBJDIFF: 같음'이어도 exe가 다를 수 있음(exe가 같은지는 gate.py의 해시로 봄). `--disasm`은 함수 전체를 견줍니다(3-0부터: 함수 안 `$LN123:` 꼬리표에서 끊지 않음) |
| `tusrc.py` | 테스트만 고친 단계에서 테스트가 보는 src가 그대로인지: 옛 트리(`--old-root`, 고치기 전 tests\·src\ 사본)와 지금 트리의 같은 테스트를 같은 cl 옵션(기본: run_regressions.bat의 CF)으로 `cl /E` 해, `#line`이 `src\`·`sensors\`·`installer\`(기본 `--dirs`)의 `.cpp`·`.h`를 가리키는 구간의 토큰을 파일 이름과 함께 차례대로 견줍니다(공백·빈 줄 무시). 가짜 `#define` 하나가 빠지면 '다름'. 일부러 건 새 가짜는 `--rename 옛이름=새이름`(바꾼 수를 찍음). 코드를 다른 src 파일로 옮긴 단계는 `--ignore-file`(파일 이름은 빼고 토큰 차례만: 옮긴 자리가 include 차례에서 같은 자리일 때) 또는 `--multiset`(차례도 빼고 토큰마다 개수만: 함수를 다른 곳에서 include되는 파일로 옮겨 차례가 바뀔 때. 더 느슨함, 개수가 다른 토큰을 찍음). 둘 다 파일별 토큰 수의 바뀜을 찍습니다. `--self-test <테스트>`는 자기 자신과 '같음', `#define` 하나 뺀 사본과 '다름'인지 봅니다. | 뜻(같은 토큰이면 같다고 봄), include하지 않는 src, src가 쓰지 않는 매크로, 테스트 자신의 코드와 Windows·C++ 헤더. 테스트 쪽 가짜·stub의 본문(tests\의 fixture 헤더)은 견주지 않음(이름이 같고 본문만 바뀐 가짜는 '같음'). `--multiset`은 토큰 차례가 바뀐 것(두 문장이 자리를 바꿈)을 못 봄 |
| `logkeys.py` | src의 `Log(` 형식 문자열(여러 줄 이어 붙이기 포함, 주석 속은 뺌)을 고정 문구·키·원문으로. `--diff REV`로 dock.log 문구가 바뀌었는지. | 실행 때 만드는 문구(`Log(L"%ls", buf)`, LogLines·LogMissing, 머리 줄), 형식 인자로 들어가는 글자 |

## 단계마다 돌리는 순서

빌드와 테스트는 한 번에 하나만 돌립니다(같은 `build\`, 고정 이름). mod.36 리팩토링의 기준 커밋은 그 리팩토링 전의 HEAD(mod.35)였습니다
(다음 리팩토링도 그때의 HEAD를 기준 커밋으로 씁니다).

1. **.cpp를 더하거나 지우거나 이름을 바꾼 단계:** `build\*.obj`를 지운 뒤 빌드합니다(gate.py가 짝 없는 .obj를 FAIL로
   잡지만, 지우는 것은 사람이 합니다). bat의 파일 목록(CLAUDE.md의 '소스 구조')도 함께 고칩니다.
2. `python tools\refactor\logkeys.py --diff <기준 커밋>` - 'Log 문구는 바꾸지 않는다'면 사라짐 0, 새로 0. mod.36 끝(시작 버튼의
   `start:` 줄과 `taskbar:`의 시작 단추 없음 줄을 일부러 바꾼 뒤)의 값은 사라짐 15, 새로 6이고, 그 뒤 단계는 앞 단계의 출력과 견줍니다.
3. 파일 사이로 옮긴 단계: `python tools\refactor\movecheck.py --from <기준 커밋>` (원본을 좁히려면 `--files src\a.cpp`) -
   'MOVE-ONLY: 예'이고, 판정 줄의 '눈으로 확인할 것'(꼴만 바뀐 줄, 짝 없는 접착 `?`, 같은 파일 안 이동, 짧은 조각)을 읽어 허용합니다.
   파일을 나누지 않고 한 파일 안에서 정리만 한 단계는 `--strict`(이것들이 하나라도 있으면 '아니오').
   테스트만 고친 단계(가짜를 공통 헤더로 옮김 등)는 고치기 전에 tests\·src\(·sensors\)를 scratch에 복사해 두고
   `python tools\refactor\tusrc.py --old-root <사본> tests\x_test.cpp ...` - 고친 테스트마다 'TUSRC: same'.
4. exe 해시가 기준과 다르면(또는 옮긴 단계면) objdiff로 원인을 봅니다.
   `python tools\refactor\objdiff.py --compile <기준 커밋> --compile-out build\refactor-scratch\obj-base`,
   `... --compile worktree --compile-out build\refactor-scratch\obj-now`,
   `python tools\refactor\objdiff.py --a build\refactor-scratch\obj-base --b build\refactor-scratch\obj-now --disasm 10`.
   '다름'마다 인라인·레지스터 배정·이름 바꿈뿐인지 읽습니다. 데이터 다름도 기본으로 판정에 들어갑니다(`OBJDIFF: 같음(코드) / 데이터
   다름 N`이면 그 기호를 읽음; 파일 사이 이동처럼 .pdata/.xdata가 함께 바뀌는 것을 알고 볼 때만 `--data-loose`).
5. 관문: `python tools\refactor\gate.py --baseline userResult\<기준 폴더> --out build\refactor-scratch\gate-<단계> --dev-compile`
   - 0-3(특성 테스트) 뒤의 단계는 `baseline-0-3`(11-20, src는 기준 커밋 그대로: 검사 수, PASS 줄, HASH 1143줄, 테스트 exe 49개).
     옛 `baseline`(mod.35, 0-1 전)은 0-3까지의 단계를 다시 볼 때만 씁니다. Windows 업데이트 뒤는 `baseline-0-3-os`,
     시작 버튼 변경(6-3) 뒤는 `baseline-6-3`(위 머리말)이라, 위 명령의 `baseline-0-3`을 지금 트리에 그대로 쓰면 FAIL입니다.
   - 해시가 같아야 하는 단계(선언·include 정리, 이름 붙이기)는 `--require-same-exe`, 검사를 더하지 않는 단계는 `--exact-counts`.
   - 버전을 올리는 마지막 단계만 `--allow-helper-change`(도우미 바이트가 바뀜).
   - 그림이나 프레임 흔적이 바뀌어도 되는 단계만 `--allow-hash-change <접두어>`(여러 번: `dock_pixels/`, `settings_pixels/`,
     `dock_trace/` ...). 그 이름의 다름은 WARN이라 끝 줄은 `GATE: PASS (부분: ...)`이고, 사람이 그림·흔적을 보고 허용합니다.
     기준 폴더에 `hash-lines.txt`가 없으면(0-3 전의 기준) HASH 대조는 INFO '기준 없음'입니다. 0-3 뒤의 기준은
     `baseline-0-3`(HASH 줄과 흔적 원문 `traces\` 포함)입니다.
   - `dock_trace/` HASH가 다르면 그 줄에 두 흔적 원문(`<기준>\traces\...`, `<--out>\traces\...`)을 견줄 diff 명령이 붙습니다.
     실제 실행은 run_regressions 전에 `build\tests` 아래의 옛 `trace-*.txt`를 지우고 끝난 뒤 `--out\traces\`로 복사합니다(약 37 MB).
   - HASH는 같은 PC에서 혼자 돌린 값끼리만 견줍니다(글꼴·GPU 드라이버에 기댐). 다른 일과 함께 돌리면 `dock_pixels/`에
     UNSTABLE이 늘거나 다른 값이 찍힐 수 있습니다.
   - 시각에 기대는 검사가 흔들리면 `--rerun-failed`로 멈춘 테스트부터 끝까지 한 번 더 돌려 흔들림인지 가립니다
     (첫 실행의 FAIL은 GATE에 남습니다).
   - 테스트 exe 해시는 판정에 넣지 않고 정보로만 찍습니다(다른 exe 이름도 찍음). 기본은 기준 폴더의 `test-exe-hashes.txt`(mod.35),
     `--test-exe-hashes <파일>`로 다른 기준을 줍니다(0-2 뒤: `test-exe-hashes-after-0-2.txt`, 0-2가 고친 테스트 6개만 다름. `baseline-0-3`의 `test-exe-hashes.txt`는 0-3 뒤의 49개).
     실제 실행(regressions나 interactions를 돌린 것)은 끝에 그 실행의 테스트 exe 해시를 `--out\test-exe-hashes.txt`에 씁니다(3-0부터).
     다음 단계에서 `--test-exe-hashes <앞 단계의 --out>\test-exe-hashes.txt`로 주면 그 단계에서 달라진 테스트 exe만 보입니다.
   - 끝 줄 `GATE: PASS`(종료 코드 0) / `GATE: FAIL (이유)`(1), 표는 `--out` 폴더의 `gate-summary.txt`에도 남습니다.
     건너뛴 구역(SKIP)이나 WARN(exit= 줄 없음, `--allow-helper-change`)이 있으면 `GATE: PASS (부분: ...)`(3)이고, 단계의 관문으로는
     꾸밈 없는 `GATE: PASS`만 통과로 칩니다. 기준 파일이 없거나 `--out`이 기준 폴더면 아무것도 돌리지 않고 2입니다.

이미 있는 로그만 다시 대조하려면 `--from-logs <폴더>`(그 폴더의 build.log, regressions.log, interactions.log, hash-after-build.txt).
이 도구가 돌린 로그 끝에는 기준 로그처럼 `exit=N` 줄이 붙고, 실제 실행은 `--out`에 hash-after-build.txt도 씁니다. 해시 파일이 없는
폴더는 FAIL이고(`--allow-no-hash`면 SKIP), `--from-logs`는 작업 트리의 옛 `build\*.obj`를 보지 않으므로 끝 줄은 늘
`GATE: PASS (부분: build/옛 build\*.obj SKIP)`(3)입니다.

## 다른 PC에서 기준을 새로 잡기

기준 해시와 검사 수(특히 매트릭스, 시작 메뉴 검사 수)는 PC마다 다를 수 있습니다. 적힌 기준은 그 리팩토링을 돌린 PC 한 대의 값입니다.

1. 기준 커밋의 트리에서(작업 트리에 src 변경이 없을 때) 아무것도 돌고 있지 않은지 확인합니다. mod.36이 커밋된 뒤에는
   기준 커밋(mod.35)의 트리에 이 도구가 없으므로, 그 뒤의 HEAD(이 도구가 있는 트리)를 기준 커밋으로 씁니다.
2. `python tools\refactor\gate.py --record --out <새 기준 폴더> --dev-compile` (빈 폴더; 기준 파일이 이미 있으면 `--force` 없이는 거부) - build → regressions → interactions를 돌려
   `build.log`, `hash-after-build.txt`, `regressions.log`, `regressions-results.txt`, `regressions-pass-lines.txt`,
   `interactions.log`, `test-exe-hashes.txt`, `hash-lines.txt`, `traces\`, `selfcheck.log`을 씁니다. 절대 조건(경고 0, 실패 0, 모든 테스트에 결과 줄)이
   깨지면 `GATE: RECORDED (단, ...)`와 종료 코드 1입니다.
3. 같은 PC에서 한 번 더 `gate.py --baseline <새 기준 폴더>`로 돌려 `GATE: PASS`(해시 재현)인지 봅니다.
4. 그 뒤 단계에서는 `--baseline <새 기준 폴더>`를 씁니다.
