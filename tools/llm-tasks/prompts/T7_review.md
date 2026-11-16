# Task T7: review a change (advice only)

Below is a change to Liquid Dock as `git diff` output. Review it like a careful senior Windows C++ developer and write your findings in Korean as a Markdown file `review.md`. A person reads it and decides; nothing is applied automatically.

Look for real problems only:
- bugs: wrong conditions, off-by-one, null or closed handles, use after free, a value used before it is set, integer overflow, wrong units (seconds and milliseconds, pixels and DIPs);
- Win32 misuse: handles or GDI objects not released, wrong message handling, blocking calls on the UI thread, missing error checks on calls that can fail;
- threads: data shared between threads without a lock or an atomic;
- tests that do not test what their text says.

Do not report style, naming, formatting or personal preferences. Do not guess about code you cannot see.

Format:
- `# 변경 검토` as the title, then one line saying which files you looked at.
- Then `## 발견 사항`: one numbered item per finding with: 심각도 (높음, 중간 or 낮음), the file and the line of the new code (from the `@@` hunk headers), what is wrong and when it goes wrong, and a suggested fix in a few lines of code if it helps.
- If you find nothing that matters, write `발견 사항 없음` and two or three sentences on what you checked.
- End with `## 확신이 없는 부분` for things you were not sure about (or `없음`).
