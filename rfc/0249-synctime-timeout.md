---
status: in-progress
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0249: Add timeout to syncTime() to prevent infinite blocking

## Motivation

`syncTime()` loops with 500 ms sleeps until `time(nullptr)` returns a value
> 57 600 (= 8 h × 2, a sentinel for "not synced yet").  If NTP servers are
permanently unreachable (captive portal, DNS blocked, route gone), the loop
spins indefinitely.  In `setup()` this causes the device to hang at boot.
In `loop()` (called from RFC-0056 periodic re-sync and transport-switch
paths) it blocks all URC / SMS processing indefinitely — the WDT kicks
prevent a reboot but the device is useless until NTP eventually replies.

## Plan

Add a 30-second deadline to the wait loop in `syncTime()`.  If the deadline
expires, log the failure and return — the caller proceeds with the
pre-existing (potentially stale) clock value.  The RFC-0079 retry logic
(every 5 min while clock is invalid) will continue trying on subsequent
loop iterations.

## Notes for handover

The 30-second budget matches the `configTime()` SNTP client's default
retry behaviour; a reply normally arrives within 2 s on a healthy network.
`s_bootTimestamp` and `s_lastNtpSyncTime` are only updated on success so
the `/status` uptime display stays correct.
