---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0074: /version command

## Motivation

When debugging a remote device the operator needs a quick way to
confirm which firmware build is running. `/status` contains uptime and
counters but not the build timestamp. A dedicated `/version` command
gives a single-line answer: "Built: Apr 10 2026 14:23:55".

## Plan

**`src/telegram_poller.h`**:
- Add `void setVersionStr(const String &v)` setter.
- Add `String versionStr_;` private member (default: "(unknown build)").

**`src/telegram_poller.cpp`**:
- Add `/version` handler: `bot_.sendMessageTo(u.chatId, versionStr_)`.
- Add `/version — Show firmware build timestamp` to `/help`.

**`src/main.cpp`**:
- Wire: `telegramPoller->setVersionStr(String("Built: ") + __DATE__ + " " + __TIME__);`
  `__DATE__` and `__TIME__` are injected by the compiler for main.cpp's TU.

**`src/telegram.cpp`**:
- Register `/version` command with description "Show firmware build timestamp".
- Update the Serial log string.

## Notes for handover

Changed: `src/telegram_poller.{h,cpp}`, `src/main.cpp`,
`src/telegram.cpp`, `rfc/0074-version-command.md`.

No new tests — pure string pass-through.
