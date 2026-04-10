---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0236: Recover URCs piggybacked on AT status-refresh responses

## Motivation

Every 30 s, the status refresh block in `loop()` issues 4 AT commands:
`AT+CSQ`, `AT+CREG?`, `AT+COPS?`, `AT+CPMS?`. TinyGSM's `waitResponse()`
reads **all** bytes from `SerialAT` into its internal buffer, so any `+CMTI`,
`RING`, or `+CLIP` URC that arrives during one of these exchanges is consumed
by TinyGSM and never seen by our dispatch loop.

The two high-level calls (`modem.getSignalQuality()`,
`modem.getRegistrationStatus()`) don't expose the raw buffer at all, making
URC recovery impossible for those two. The raw calls (`+COPS?`, `+CPMS?`)
capture to a `String` but we were not scanning it.

Effect: an SMS that arrives during the ~100–400 ms AT exchange window is
silently dropped. RFC-0235's 30-min periodic sweep eventually recovers it,
but real-time delivery is broken for that window.

## Plan

1. Replace `modem.getSignalQuality()` with `realModem.sendAT("+CSQ")` +
   `realModem.waitResponse(data)`, then parse `+CSQ: <rssi>,<ber>` manually.
2. Replace `modem.getRegistrationStatus()` with `realModem.sendAT("+CREG?")` +
   `realModem.waitResponse(data)`, then parse `+CREG: <n>,<stat>` manually
   (the +CREG stat values map 1-to-1 onto TinyGSM's `RegStatus` enum).
3. Accumulate all 4 raw response strings into a single `s236raw` buffer.
4. At the end of the 30 s block, scan `s236raw` line-by-line for piggybacked
   `+CMTI:`, `RING`, and `+CLIP:` lines; dispatch each via
   `smsHandler.handleSmsIndex()` / `callHandler.onUrcLine()`.
   Add `esp_task_wdt_reset()` before each `handleSmsIndex()` call since
   forwarding a message involves a Telegram HTTP round-trip.

## Notes for handover

`modem_.waitResponse(timeout, data)` in TinyGSM accumulates every byte read
from the serial stream into `data`, including URCs — this is the mechanism
that makes the scanning possible. The timeout is 2 s per command; a typical
modem response arrives in <50 ms, so the total exposure window per loop
iteration is ~100–200 ms in practice.
