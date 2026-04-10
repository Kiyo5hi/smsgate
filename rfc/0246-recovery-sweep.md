---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0246: Recovery sweep on consecutiveFailures drop to 0

## Motivation

When Telegram is temporarily unreachable, `handleSmsIndex` fails for each
`+CMTI` URC, incrementing `consecutiveFailures_`.  When Telegram comes
back up, only the single +CMTI that triggered the first successful
`handleSmsIndex` is processed.  Any OTHER SMS that arrived on the SIM
during the outage sit there until:

- The 30-min periodic sweep (RFC-0235) — up to 30 min delay
- The next WiFi/transport reconnect event (RFC-0243)
- A manual `/sweep` command

The gap: after Telegram recovers, there is no mechanism to immediately
drain the SIM backlog unless one of the above events coincidentally fires.

## Plan

At the end of each `loop()` iteration (just before `delay(50)`):

1. Check if `smsHandler.consecutiveFailures()` just dropped from > 0 to 0.
   (`consecutiveFailures_` is reset to 0 on each successful Telegram send.)
2. If the transition is detected AND transport is not `kNone`:
   - Log the event
   - `esp_task_wdt_reset()` + `sweepExistingSms()` + `esp_task_wdt_reset()`
3. Update the saved value AFTER the (potential) sweep so the transition
   is not re-triggered by the sweep's own successful forwards.

## Notes for handover

The check fires at most once per failure→recovery cycle.  If the sweep
itself finds no SMS (SIM was already clean), `AT+CMGL` returns immediately
with no entries and the sweep returns 0 — negligible overhead.

False positive scenario: a one-shot `+CMTI` succeeds on the first ever
loop (s_prev = 0, cur = 0 → `s_prev > 0` guard fails → no trigger).
The guard `s_prev > 0` ensures the sweep only fires when there was
actually a failure window preceding the recovery.
