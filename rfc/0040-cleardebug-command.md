---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0040: /cleardebug command

## Motivation

The `/debug` command dumps the last 20 SMS diagnostic entries, but there was no
way to clear the log. After resolving a truncation or encoding issue the old
entries are noise. `/cleardebug` wipes both the in-RAM ring and the NVS blob so
the next `/debug` starts fresh.

## Plan

**`src/sms_debug_log.h`** — add inline `clear()`:
```cpp
void clear() {
    head_   = 0;
    count_  = 0;
    if (persist_)
        persist(); // writes empty blob to NVS
}
```

**`src/telegram_poller.cpp`** — add handler after `/debug`:
```cpp
if (lower == "/cleardebug") {
    if (debugLog_) {
        debugLog_->clear();
        bot_.sendMessageTo(u.chatId, String("\xF0\x9F\x97\x91 Debug log cleared."));
    } else {
        bot_.sendMessageTo(u.chatId, String("(debug log not configured)"));
    }
    return;
}
```

**`src/telegram.cpp`** — register `/cleardebug` command and bump
`DynamicJsonDocument` to 1024 to accommodate the extra command object.

## Notes for handover

Only `src/sms_debug_log.h`, `src/telegram_poller.cpp`, and `src/telegram.cpp`
changed. No new tests needed — the `clear()` path is exercised indirectly by
the existing debug-log persistence tests.
