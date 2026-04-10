---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0058: /last [N] command for condensed SMS history

## Motivation

`/debug` dumps all 20 log entries with full PDU details — too verbose for a
quick "what SMS arrived recently?" check. `/last` shows the most recent N
entries in a one-line-per-SMS format without PDU prefixes or concat metadata.

## Plan

**`src/sms_debug_log.h`** — add method:
```cpp
String dumpBrief(size_t n = 5) const;
```

**`src/sms_debug_log.cpp`** — implement: walks `n` entries newest-first,
formats each as `YYYY-MM-DD HH:MM | +1234567890 | fwd 160c`.

**`src/telegram_poller.cpp`** — add handler before `/debug`:
```cpp
if (lower == "/last" || lower.startsWith("/last ")) {
    size_t n = 5; // parse optional arg
    bot_.sendMessageTo(u.chatId, debugLog_->dumpBrief(n));
}
```

**`src/telegram.cpp`** — register `/last` command.

## Example

```
/last 3 →
2026-04-10 14:32 | +8613800138 | fwd 160c
2026-04-10 12:15 | +1234567890 | fwd 42c
2026-04-09 23:01 | +8613800138 | fwd 70c
```

## Notes for handover

`src/sms_debug_log.{h,cpp}`, `src/telegram_poller.cpp`, `src/telegram.cpp`
changed. No test changes needed.
