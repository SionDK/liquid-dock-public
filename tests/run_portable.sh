#!/bin/sh
# The tests that need no Windows, with g++ or clang++ (C++20): Linux, WSL, macOS, a local LLM box.
#   tests/run_portable.sh            (CXX=clang++ tests/run_portable.sh for clang)
set -e
cd "$(dirname "$0")/.."
CXX=${CXX:-g++}
out=build/tests-portable
mkdir -p "$out"
for t in fluent_logic fluent_settings icon_pixels admin_protocol app_click taskbar_apps tray_buttons battery start_menu sys_poll idle_wait refreshed sensor_logic; do
  $CXX -std=c++20 -O1 -Wall -Wextra "tests/${t}_test.cpp" -o "$out/${t}_test"
  "$out/${t}_test" >"$out/${t}_test.txt" || { cat "$out/${t}_test.txt"; echo "FAILED: ${t}_test"; exit 1; }
  tail -n 1 "$out/${t}_test.txt"
done
$CXX -std=c++20 -O1 -Wall "tests/hwinfo_test.cpp" src/hwinfo.cpp -o "$out/hwinfo_test"
"$out/hwinfo_test"
# tests written with the local-LLM task pack (tools/llm-tasks, run_tasks.py --apply), if there are any
for f in tests/llm/*_test.cpp; do
  [ -e "$f" ] || continue
  t=$(basename "$f" .cpp)
  $CXX -std=c++20 -O1 -Wall -I. "$f" src/hwinfo.cpp -o "$out/$t"
  "$out/$t" >"$out/$t.txt" || { cat "$out/$t.txt"; echo "FAILED: $t"; exit 1; }
  tail -n 1 "$out/$t.txt"
done
echo "all portable tests passed"
