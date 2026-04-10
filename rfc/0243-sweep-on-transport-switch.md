---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0243: Sweep SIM on cellular→WiFi and kNone→WiFi transport switches

## Motivation

`sweepExistingSms()` is called in two places:
- `setup()` end — handles SMS that arrived while the bridge was offline
- WiFi-drop/reconnect path in `loop()` — handles SMS that piled up while
  WiFi was down and Telegram was unreachable

But two other transport transitions were not covered:

1. **Cellular → WiFi recovery** (`kCellular` path in the transport-check
   block): when WiFi recovers after a cellular-fallback period, any SMS
   that `handleSmsIndex` failed to forward on the cellular path (TLS
   issues, network problems specific to cellular) are still on the SIM.
   Without a sweep, they wait up to 30 min for the periodic sweep (RFC-0235).

2. **`kNone` → WiFi establishment** (first successful transport on a device
   that had no connectivity at boot): if WiFi wasn't available at boot,
   `s_needBootSweep` is set and the RFC-0238 `onPollSuccessFn` lambda runs
   the sweep after the first successful poll. But that lambda fires after
   the first `pollUpdates` round-trip, which may take 3 s+ after transport
   is established. A direct sweep immediately after `setupTelegramClient`
   confirms the transport is alive ensures faster delivery.

## Plan

Add `esp_task_wdt_reset()` + `smsHandler.sweepExistingSms()` +
`esp_task_wdt_reset()` immediately after each:
1. `activeTransport = kWiFi` in the cellular recovery `wifiBeginPending`
   branch.
2. `activeTransport = kWiFi` in the `kNone` `wifiBeginPendingNone` branch.

## Notes for handover

The kNone case: if `s_needBootSweep` is still set, the `onPollSuccessFn`
lambda (RFC-0238) will also call `sweepExistingSms()` on the first
successful `pollUpdates`. That's harmless — a second `AT+CMGL` when the
SIM is already empty is fast (< 50 ms). No deduplication guard needed.
