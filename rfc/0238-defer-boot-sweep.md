---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0238: Defer boot sweep when Telegram is unreachable at boot

## Motivation

`sweepExistingSms()` is called at the end of `setup()`, immediately after
the boot banner. If Telegram is unreachable at boot (server restart, API
rate-limit, transient outage), each SMS forwarded by the sweep hits
`sendMessageReturningId()` → failure → `noteTelegramFailure()` counter++.
After 8 failures the device reboots. On reboot, the SIM still holds the
same SMS, and the sweep fires again → boot loop.

The boot banner send (`realBot.sendMessage(bootMsg)`) returns a bool that
indicates whether Telegram is reachable. We currently ignore it.

## Plan

1. Capture the boot banner send result.
2. If the send succeeds (Telegram is reachable), proceed with
   `sweepExistingSms()` immediately as before.
3. If the send fails, set `s_needBootSweep = true` and skip the sweep.
   SMS remain safely on the SIM.
4. In the `onPollSuccessFn_` callback (fires on every successful
   `pollUpdates` round-trip), check `s_needBootSweep`. On the first
   successful poll, clear the flag and call `sweepExistingSms()`.
   Add `esp_task_wdt_reset()` around the sweep since it may take several
   Telegram API calls.

## Notes for handover

The `setOnPollSuccessFn` lambda already has access to `smsHandler` (file-scope
static in main.cpp). The sweep is safe to call from that lambda — the TLS
connection is known-good (we just finished a `pollUpdates`) and the modem is
idle after the recent AT exchange. `s_needBootSweep` is a file-scope bool
initialised to false; set to true if the boot banner send fails.
