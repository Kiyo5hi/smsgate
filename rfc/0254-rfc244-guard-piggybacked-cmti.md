---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0254: Apply RFC-0244 no-transport guard to piggybacked +CMTI dispatch

## Motivation

RFC-0244 guards the main URC drain loop: when `activeTransport == kNone`,
`+CMTI` URCs are skipped rather than dispatched to `handleSmsIndex()`.
The rationale is that `handleSmsIndex()` calls `doSendMessage()` which
fails and increments `SmsHandler::noteTelegramFailure()`. With no
transport, a burst of 8+ SMS would exhaust the 8-failure limit and
trigger a reboot loop.

However, the RFC-0236 piggybacked dispatch block (30 s status refresh
AT responses) and the RFC-0239 piggybacked dispatch block (modem health
check AT response) were missing this guard. Both blocks called
`smsHandler.handleSmsIndex(idx)` unconditionally when a piggybacked
`+CMTI` was found, regardless of transport state.

## Plan

Add the same `activeTransport == kNone` check to both piggybacked
dispatch sites:

```cpp
if (activeTransport == ActiveTransport::kNone) {
    Serial.printf("[RFC-0236] Piggybacked +CMTI idx=%d skipped (no transport)\n", idx);
} else {
    esp_task_wdt_reset();
    smsHandler.handleSmsIndex(idx);
}
```

The SMS stays on the SIM and will be found by `sweepExistingSms()` when
transport becomes available (RFC-0243/RFC-0246/RFC-0253).

## Notes for handover

The RFC-0245 re-arm sweep is unconditionally guarded by
`if (activeTransport != ActiveTransport::kNone)`, so it already
respects transport state.  This RFC brings RFC-0236 and RFC-0239 into
the same pattern.
