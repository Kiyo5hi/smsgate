---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0042: /ping command

## Motivation

There is no instant way to verify the bridge is alive and processing Telegram
commands. `/status` works but is verbose. `/ping` → "🏓 Pong" gives a cheap
round-trip liveness signal without rendering the full status block.

## Plan

**`src/telegram_poller.cpp`** — add handler before `/debug`:
```cpp
if (lower == "/ping") {
    bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x8F\x93 Pong")); // 🏓
    return;
}
```

**`src/telegram.cpp`** — register `/ping` command in `setMyCommands` and
update the Serial log line.

## Notes for handover

Only `src/telegram_poller.cpp` and `src/telegram.cpp` changed. No test
changes needed.
