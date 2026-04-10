---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0110 — /resetstats command

## Motivation

Session counters (SMS forwarded, failed, calls received, outbound sent/failed)
accumulate since boot and show in `/status`. When investigating a particular
incident, the operator may want to reset these counters to zero to isolate
activity from a known-good baseline — without rebooting the device.

## Plan

1. Add `resetStats()` methods to `SmsHandler` (zeroes `smsForwarded_`,
   `smsFailed_`, `smsBlocked_`, `smsDeduplicated_`, `consecutiveFailures_`),
   `CallHandler` (zeroes `callsReceived_`), and `SmsSender` (zeroes
   `sentCount_`, `failedCount_`).

2. Add a `setResetStatsFn(std::function<void()>)` setter to `TelegramPoller`.
   Production wires a lambda that calls the three `resetStats()` methods.

3. Add a `/resetstats` handler: call `resetStatsFn_()` and reply with
   `"✅ Session counters reset."`. Admin-only gate via the existing `mutator_`
   pattern.

4. Actually: make it available to all authorized users (not admin-only)
   since it's diagnostic-only and has no side effects on stored state.

5. Tests for the handler.

## Notes for handover

Lifetime counters (NVS-persisted forwarded count `s_lifetimeFwdCount`,
boot count) are intentionally NOT reset — those are permanent history. Only
the in-RAM session counters are affected.
