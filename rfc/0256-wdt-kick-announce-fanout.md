---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0256: WDT kick per recipient in /announce broadcast loop

## Motivation

The `announceFn_` lambda in `main.cpp` (wired via `setAnnounceFn`) loops over
all `allowedIds` (up to 10) and calls `realBot.sendMessageTo()` for each
without any WDT kick.

Each `sendMessageTo()` can block up to ~23 s (15 s TCP + 4 s header + 4 s
body drain).

10 × 23 s = **230 s** from the per-update WDT kick in `tick()` to completion,
which would trip the 120 s watchdog every time Telegram is slow and more than
~5 users are configured.

## Plan

Add `esp_task_wdt_reset()` at the top of the fan-out loop in `announceFn_`:

```cpp
for (int i = 0; i < allowedIdCount; i++) {
    esp_task_wdt_reset(); // RFC-0256
    if (realBot.sendMessageTo(allowedIds[i], text))
        count++;
}
```

`main.cpp` is only compiled for the ESP platform (excluded from native via
`build_src_filter`), so no `#ifdef ESP_PLATFORM` guard is needed.

## Notes for handover

The per-update kick in `tick()` covers the start of `processUpdate()`.
The new per-recipient kick in `announceFn_` covers the fan-out loop.
Combined, the worst-case gap is now a single `sendMessageTo()` duration
(~23 s), regardless of how many users are in the allow list.
