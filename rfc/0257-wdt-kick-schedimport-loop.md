---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0257: WDT kick per line in /schedimport recursive dispatch loop

## Motivation

The `/schedimport` handler in `telegram_poller.cpp` loops over each line of
the import body and calls `processUpdate(syn)` recursively for each
`/schedulesend`, `/scheduleat`, or `/recurring` line.

Each recursive `processUpdate()` call sends one `bot_.sendMessageTo()`
confirmation message, which can block ~23 s.  With `kScheduledQueueSize = 5`
slots:

- 5 recursive calls × ~23 s = 115 s
- Plus the final summary reply (+23 s) = **138 s** total

138 s > 120 s WDT timeout.

The per-update kick in `tick()` only covers the outermost `processUpdate()`
call for `/schedimport`, not the recursive ones inside the loop.

## Plan

Add `esp_task_wdt_reset()` before each `processUpdate(syn)` call in the
`/schedimport` import loop:

```cpp
esp_task_wdt_reset(); // RFC-0257
processUpdate(syn);
```

`telegram_poller.cpp` is only compiled for the ESP platform (excluded from
native builds via `build_src_filter`), so no `#ifdef ESP_PLATFORM` guard is
needed.

## Notes for handover

The `processUpdate()` calls in the loop dispatch only `/schedulesend`,
`/scheduleat`, or `/recurring` commands (guarded by the `allowed` prefix
check above the call site), so the recursive depth is exactly 1 — no
further recursion occurs from those handlers.

With the per-line kick, the worst-case gap is a single `sendMessageTo()`
duration (~23 s), regardless of how many lines the import contains.
