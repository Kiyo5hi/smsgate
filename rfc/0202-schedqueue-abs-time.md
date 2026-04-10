---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0202: Absolute UTC time in scheduled SMS replies

## Motivation

`/schedqueue` shows "in 30m", `/schedulesend` confirmation shows "in 30
min", and `/schedinfo` shows "ETA: 30 min". Relative times are
ambiguous after even a short distraction. When NTP has synced, showing
the absolute UTC send time (e.g. "14:32 UTC") alongside the relative
minutes removes the ambiguity.

## Design

Add a `setWallTimeFn(std::function<long()> fn)` to TelegramPoller
(returns `time_t`/`long` UTC seconds; 0 or negative = NTP not synced).

Add a file-static helper `schedEta(sendAtMs, nowMs, wallTimeFn)` inside
`telegram_poller.cpp` that returns a compact string:
- If `wallTimeFn` is set and returns > 1000000000: `"in Xm (HH:MM UTC)"`
  - If `sendAtMs <= nowMs` (already past): `"now (overdue)"`
  - If send is > 24h away: include the date: `"in Xm (MM/DD HH:MM UTC)"`
- Fallback: `"in Xm"` (NTP not available)

Use `schedEta` in:
- `/schedulesend` confirmation reply
- `/schedqueue` slot list
- `/schedinfo` ETA line

## Notes for handover

- `wallTimeFn_` in `TelegramPoller` is an `std::function<long()>`.
- In main.cpp: `telegramPoller->setWallTimeFn([]() { return (long)time(nullptr); });`
- `schedEta` lives in the anonymous namespace at the top of
  `telegram_poller.cpp` (not in the header — it's an implementation detail).
- The time format uses `gmtime` + `strftime` (both available on ESP32 and
  in the host test environment via the C standard library).
