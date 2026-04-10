---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0211: Quiet hours — defer scheduled SMS during configured UTC window

## Motivation

A scheduled SMS set to fire at "in 8 hours" may land at 03:00 local
time if the operator forgot to account for timezone. Quiet hours lets
the operator configure a UTC window (e.g. 22:00–08:00) during which
scheduled SMS delivery is silently deferred until the window ends.

## Design

New commands:
- `/setquiethours <start>-<end>` — set quiet hours window in UTC
  (24h format, e.g. `/setquiethours 22-08`). Stored as two bytes in
  NVS key `"quiet_start"` and `"quiet_end"`.
- `/clearquiethours` — disable quiet hours.
- `/quiethours` — show current setting.

In TelegramPoller `tick()`, before firing a scheduled slot:
```cpp
if (isInQuietHours(wallTimeFn_)) continue; // skip this tick cycle
```

`isInQuietHours` checks `wallTimeFn_() % 86400 / 3600` against the
configured [start, end) window (with wrap-around for overnight ranges).
Returns false if quiet hours not configured or NTP not synced.

Quiet hours are NOT persisted via `persistSchedFn_` — they use a
dedicated NVS key pair. They affect ALL scheduled slots simultaneously.

## Notes for handover

- Implemented via two new members on TelegramPoller: `quietStart_`
  and `quietEnd_` (both int, -1 = disabled). Set via setter
  `setQuietHours(int start, int end)` / `clearQuietHours()`.
- `setQuietHoursConfigFn` not needed — commands mutate the members
  directly. main.cpp loads from NVS at boot and wires to persistence.
- A slot that was suppressed by quiet hours will fire on the next
  `tick()` after the window ends — no special wakeup needed.
