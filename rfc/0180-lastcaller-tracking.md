---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0180: Last-caller tracking in CallHandler

## Motivation

`/callstatus` shows the call state and config, but not who last called.
Adding `lastCallerNumber()` and `lastCallTimeMs()` to `CallHandler` lets
the `/callstatus` output show: "Last call: +447911123456 (3h ago)".

## Plan

1. **`CallHandler`** — add private `lastCallerNumber_` (String) and
   `lastCallTimeMs_` (uint32_t). Updated in `commitRinging()` just before
   the Telegram send. Exposed via `lastCallerNumber()` and `lastCallTimeMs()`.

2. **`main.cpp`** — update the `/callstatus` lambda to include:
   ```
     Last caller: +447911123456 (or "(none yet)")
     Last call: 3h ago (or "(none)")
   ```

3. **Tests** — verify `lastCallerNumber()` and `lastCallTimeMs()` are set
   after a committed call event.

## Notes for handover

- `lastCallTimeMs()` returns `clock_()` at commit time (millis since boot).
  The lambda in main.cpp computes "X ago" by subtracting from current millis.
- Empty string means no call yet; "(Unknown)" means withheld.
