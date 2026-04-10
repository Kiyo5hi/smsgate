---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0241: WDT kicks in URC drain loop and transport-check sweep

## Motivation

Two locations in `loop()` call long-blocking functions without an
`esp_task_wdt_reset()`, creating a theoretical WDT trip path:

1. **URC drain loop** (`while (SerialAT.available())`): when a `+CMTI`
   is dispatched, `smsHandler.handleSmsIndex(idx)` is called with no
   prior WDT kick.  Each call takes ~10 s (AT+CMGR round-trip + Telegram
   HTTPS round-trip).  If the modem's firmware emits a burst of `+CMTI`
   URCs on wake-up (e.g. after an extended offline period), the drain
   loop processes them back-to-back without resetting the watchdog.
   With N=13 URCs at 10 s each = 130 s > 120 s WDT limit.

2. **Transport-check WiFi reconnect sweep** (`wifiDownLastCheck` branch):
   `esp_task_wdt_reset()` is called before `sweepExistingSms()` but NOT
   after.  On a full SIM the sweep may send N × Telegram roundtrips; any
   N ≥ 13 trips the WDT.

## Plan

1. Add `esp_task_wdt_reset()` immediately before each
   `smsHandler.handleSmsIndex(idx)` call in the `while (SerialAT.available())`
   drain loop in `loop()`.
2. Add `esp_task_wdt_reset()` immediately after the
   `smsHandler.sweepExistingSms()` call in the WiFi-reconnect branch of
   the transport-check block.

## Notes for handover

The drain loop already had a WDT kick at the top of `loop()` itself, but
that only covers the first SMS in a burst.  The per-item kick is the safe
pattern — already used in the RFC-0236 piggybacked-URC dispatch block and
in the RFC-0240 processUpdate loop.  Consistent application across all
`handleSmsIndex` call sites makes future reviews trivially auditable.
