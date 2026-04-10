---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0115 — NTP sync timestamp in /status

## Motivation

When the device's clock drifts or NTP fails silently, `/status` shows
`(no NTP)` for the current time but gives no indication of when the
last sync occurred. An operator debugging a stale clock wants to know
"was it synced 30 minutes ago or 3 days ago?". Tracking and displaying
the last successful NTP sync time closes this gap.

## Plan

1. Add `static time_t s_lastNtpSyncTime = 0;` to `main.cpp`.

2. In `syncTime()` (or the `s_lastNtpSyncTime` update site in the NTP
   retry block), after a successful sync: `s_lastNtpSyncTime = time(nullptr);`

3. In `statusFn`, after the `"  Time: "` line, add:
   ```
     Last NTP: 2026-04-10 14:30 UTC+8  (or "(never synced)")
   ```
   Format the timestamp using the same `strftime` + timezone offset as
   the main time display.

4. No test needed — this is `main.cpp`-only glue, not testable from
   the native env.

## Notes for handover

`s_lastNtpSyncTime` starts at 0. If still 0 when `/status` is called,
show `"(never synced)"` rather than trying to format epoch 0.
