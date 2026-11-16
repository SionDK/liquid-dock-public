# Task T6: a guide to the dock's log messages, in Korean

Write `docs/log-messages.md`: a guide in Korean for users who open `dock.log` (in Liquid Dock's `data` folder) and want to know what a line about FluentFlyout or the tray icons means and what to do about it. The users are not programmers.

Below is the list of messages, each with the code right before it, and the README sections about the two features.

Format:
- Start with a title line `# 로그 메시지 안내` and two or three sentences on where dock.log is (the `data` folder next to LiquidDock.exe), that it is rewritten every time the dock starts, and how to read a line (time, then the message).
- Then one section per area: `## FluentFlyout 연동` and `## 트레이 아이콘`.
- In each section, one Markdown table with the columns `| 메시지 | 뜻 | 할 일 |`.
- One row per message in the list. The first cell is the message key exactly as listed, inside backticks, for example `` `fluentflyout: no longer running` `` (copy every key character for character, including the `…` marks, which stand for values filled in when the line is written). Use each key once.
- `뜻`: what happened, in one or two plain Korean sentences. `할 일`: what the user can do, or `없음 (정상)` when the line is normal.
- No other backticked text that starts with `fluentflyout:` or `tray host:` (do not invent messages).
- Do not use the `|` character inside a cell.
- End with a short section `## 문제를 알릴 때` saying to attach dock.log and FluentFlyout's log (its `logs` file in FluentFlyout's settings folder), and that `python tools/loglens.py dock.log` gives a summary if that tool exists.

Write natural Korean in the polite form (합니다/하세요), the way the README is written. Keep the English message keys as they are.

Wrap the whole document in a code block that starts with four backticks and `markdown` (````markdown) and ends with four backticks, so that any code block inside it stays intact.
