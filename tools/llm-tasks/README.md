# 로컬 LLM 작업 팩

로컬 LLM(Ollama, LM Studio, llama.cpp)으로 돌려도 충분한 작업을 따로 모아 둔 곳입니다. 작업마다 **결과를 자동으로 검사**하고, 실패하면 실패 내용을 모델에 돌려주고 다시 받습니다. 검사를 통과한 결과만 `--apply`로 저장소에 들어갑니다.

여러 파일을 함께 고쳐야 하거나 Windows 창 동작을 알아야 하는 일(버그 수정, 최적화, 구조 변경)은 여기 넣지 않았습니다. 파일 하나로 끝나고, 명세가 분명하고, 기계가 맞는지 확인할 수 있는 일만 넣었습니다.

## 작업 목록

| 작업 | 결과 파일 | 검사 | 권장 모델 |
|---|---|---|---|
| T1 | `tests/llm/fluent_logic_more_test.cpp` (FluentFlyout 모니터·재생·재연결 판단 추가 테스트) | 빌드·실행, 검사 30개 이상, 변이 12개 중 75% | 14B 이상 |
| T2 | `tests/llm/fluent_settings_more_test.cpp` (FluentFlyout 설정 파일 읽기 경계 테스트) | 빌드·실행, 검사 25개 이상, 변이 11개 중 75% | 14B 이상 |
| T3 | `tests/llm/icon_pixels_more_test.cpp` (트레이 아이콘 픽셀 처리 테스트) | 빌드·실행, 검사 20개 이상, 변이 9개 중 75% | 7B 이상, **처음 시험용** |
| T4 | `tests/llm/hwinfo_more_test.cpp` (CPU 온도 읽기 경계 테스트) | 빌드·실행, 검사 25개 이상, 변이 12개 중 75% | 14B 이상 |
| T5 | `tools/loglens.py` (dock.log·FluentFlyout 로그 요약 도구) | 샘플 로그 정답 + 매번 새로 만든 로그로 모든 값 비교 | 14B 이상 |
| T6 | `docs/log-messages.md` (로그 메시지 한국어 안내서) | 메시지 33개가 빠짐없이 표에 있는지, 지어낸 메시지가 없는지, 한국어인지 | 14B 이상, 한국어 잘하는 모델 |
| T7 | `out/T7/review.md` (커밋 전 변경 검토) | 없음 (참고용, 사람이 판단) | 32B 이상이면 더 좋음 |

**변이 검사**: 테스트를 코드의 "일부러 망가뜨린 사본"(예: `<`를 `<=`로, 기본값 2를 0으로)에 대고도 돌립니다. 좋은 테스트는 그 망가짐을 잡아내야 하므로, `Check(true, ...)`만 잔뜩 쓴 테스트는 통과하지 못합니다. 살아남은 변이는 설명과 함께 모델에 돌려줍니다.

## 준비 (한 번만)

1. **Python 3.8 이상**: `py --version`으로 확인하고, 없으면 `winget install Python.Python.3.12`.
2. **C++ 컴파일러**: 독을 빌드할 때 쓰는 Visual Studio 2022 Build Tools를 그대로 씁니다(자동으로 찾음). 없으면 `pip install ziglang` 후 `--cxx zig`.
3. **로컬 LLM 서버** (아래 중 하나)
   - **Ollama** (기본): `ollama pull qwen2.5-coder:14b`. 실행기가 Ollama 자체 API로 문맥 크기(`--num-ctx`, 기본 16384)를 요청마다 지정하므로 따로 설정할 것은 없습니다.
   - **LM Studio**: 모델을 불러올 때 Context Length를 16384 이상으로, Developer 탭에서 서버 켜기(기본 포트 1234). 실행할 때 `--api openai`.
   - **llama.cpp**: `llama-server -m 모델.gguf -c 16384 --port 8080`, 실행할 때 `--api openai --base-url http://127.0.0.1:8080/v1`.

**모델 고르기** (2026-10-23 기준 정보, 가정 포함)

- VRAM 12GB GPU에는 `qwen2.5-coder:14b`(Q4 약 9GB)가 기준입니다. 16K 문맥의 KV 캐시가 3GB 안팎이라 일부가 CPU로 넘어가 느려질 수 있습니다. 그러면 KV 캐시를 반으로 줄이세요(Ollama 트레이 아이콘에서 Quit 후 다시 실행해야 적용):
  ```bat
  setx OLLAMA_FLASH_ATTENTION 1
  setx OLLAMA_KV_CACHE_TYPE q8_0
  ```
- 시스템 메모리가 32GB 이상이면 `qwen3-coder:30b`(MoE, 한 번에 3.3B만 계산)도 일부를 CPU에 두고 쓸 만한 속도로 돌아갑니다. 14B가 변이 검사를 자주 못 넘기면 이쪽을 시험해 보세요.
- 메모리 계산과 설정값은 아래 출처를 따랐습니다. 실제 속도는 직접 `--tasks T3`로 재 보는 것이 가장 정확합니다.

## 실행

저장소 폴더에서 (명령 프롬프트와 PowerShell 모두 같음. `python`이 없다고 나오면 `py`):

```bat
python tools\llm-tasks\run_tasks.py --list
python tools\llm-tasks\run_tasks.py --tasks T3 --model qwen2.5-coder:14b
```

T3(가장 작은 작업)이 통과하면 설정이 맞는 것입니다. 그다음 전부 돌리거나, 고른 작업만 돌리면서 통과한 결과를 제자리에 복사합니다:

```bat
python tools\llm-tasks\run_tasks.py --model qwen2.5-coder:14b
python tools\llm-tasks\run_tasks.py --model qwen2.5-coder:14b --tasks T1,T2 --apply
```

LM Studio라면 `--api openai --base-url http://127.0.0.1:1234/v1 --model <불러온 모델 이름>`을 붙입니다. 작업 하나에 몇 분씩 걸립니다(모델이 답을 쓰는 시간이 대부분, 12GB GPU와 14B 모델 기준 추정).

| 옵션 | 뜻 |
|---|---|
| `--apply` | 통과한 결과를 저장소의 제자리(`tests/llm/`, `tools/`, `docs/`)에 복사 |
| `--max-attempts N` | 실패 후 다시 받는 횟수 (기본 3) |
| `--num-ctx N` | Ollama 문맥 크기 (기본 16384). 프롬프트가 크면 실행기가 더 큰 값을 권합니다 |
| `--verify-only` | 모델 없이 `out\`에 있는 결과만 다시 검사 (직접 고친 뒤 확인할 때) |
| `--dry-run` | 프롬프트만 `out\<작업>\prompt.md`로 저장 (다른 도구에 붙여 넣을 때) |
| `--no-mutants` | 변이 검사 생략 (빠르지만 테스트 품질 확인이 약해짐) |
| `--diff-base REF` | T7이 검토할 기준 (기본 `HEAD` = 커밋 안 한 변경, 마지막 커밋은 `HEAD~1`) |
| `--self-check` | 작업 팩 점검: 문맥 파일, 변이 위치, 검사 스크립트 (src를 고친 뒤) |

## 결과 보기

- `tools\llm-tasks\out\report.md`: 작업별 통과 여부, 시도 횟수, 시간, 메모.
- `out\<작업>\`: 보낸 프롬프트(`prompt.md`), 모델의 답(`answer-1.md`...), 결과 파일, 마지막 검사 내용(`verify.txt`).
- 메모에 `SUSPECT`가 보이면 모델이 "코드가 틀린 것 같다"고 표시한 곳입니다. 테스트 파일에서 `SUSPECT:` 줄을 찾아 직접 확인하세요. 진짜 버그일 수도 있습니다.
- `--apply`로 들어간 테스트는 `tests\run_regressions.bat`(Windows)와 `tests/run_portable.sh`(Linux·WSL)가 자동으로 함께 빌드해서 돌립니다. `git diff`로 확인한 뒤 커밋하세요.
- `tools/loglens.py`가 생기면: `python tools\loglens.py "%LOCALAPPDATA%\Programs\LiquidDock\data\dock.log"` (PowerShell에서는 `"$env:LOCALAPPDATA\Programs\LiquidDock\data\dock.log"`). FluentFlyout 로그 파일도 뒤에 이어 넣을 수 있습니다.

## 안전

- 생성된 코드는 실행 전에 파일 쓰기·삭제, 프로세스 실행, 네트워크, 레지스트리 호출을 찾아보고, 있으면 실행하지 않고 다시 받습니다.
- 빌드와 실행은 `tools\llm-tasks\out\` 안에서 합니다. `--apply` 전에는 저장소 파일을 바꾸지 않습니다.
- 완벽한 격리(샌드박스)는 아닙니다. 처음 써 보는 모델이면 `out\<작업>\`의 결과를 한 번 읽어 보세요.
- 샘플 로그(`fixtures\`)는 실제 로그에서 사용자 이름을 `USER`로 바꾼 것입니다.

## 자주 생기는 문제

| 증상 | 해결 |
|---|---|
| `API에 연결할 수 없습니다` | Ollama가 켜져 있는지(트레이 아이콘, 또는 `ollama serve`), LM Studio 서버가 켜져 있는지 |
| `API 오류 404 ... not found` | `ollama pull <모델 이름>`, 이름 철자 (`ollama list`로 확인) |
| 답이 중간에 끊기거나 엉뚱함 | 문맥이 모자란 경우가 많습니다: `--num-ctx 24576`, LM Studio는 Context Length 늘리기 |
| 아주 느림 | GPU 메모리 부족: 위의 KV 캐시 설정, 더 작은 모델, 또는 `--no-mutants` |
| 변이 검사를 계속 못 넘김 | `--max-attempts 5`, 더 큰 모델, 또는 `verify.txt`의 살아남은 변이 목록을 보고 테스트를 직접 보충한 뒤 `--verify-only` |
| `--self-check`가 "변이 ...: 찾는 문자열이 0번" | src가 바뀌어 그 변이를 만들 수 없음: `tasks.json`의 `find`/`replace`를 새 코드에 맞게 고치기 |

## 작업 추가하기

`tasks.json`에 항목 하나를 더하고 `prompts\`에 프롬프트를 씁니다.

- `context`: 프롬프트에 붙일 파일(`file`, 필요하면 `from`/`to`로 일부만) 또는 명령 출력(`cmd`).
- `verify.kind`: `cpp-test`(빌드·실행·`ALL PASSED`·최소 검사 수·변이), `script`(검사 스크립트의 종료 코드), `none`(검사 없음).
- 변이: `{"id", "file", "find", "replace", "desc"}`. `find`는 그 파일에 정확히 한 번 있어야 하고, `desc`는 모델이 읽는 설명입니다(어떤 입력에서 무엇이 틀려지는지).

출처: [Ollama 문맥 크기 설정 (fast.io, 2026-09-11 확인)](https://fast.io/resources/ollama-context-window.md), [Ollama KV 캐시 양자화 (SSD Nodes, 2026-10-21)](https://www.ssdnodes.com/learn/ollama-kv-cache-quantization), [VRAM별 Ollama 모델 (Morph, 2026-10-16 갱신)](https://www.morphllm.com/best-ollama-models)
