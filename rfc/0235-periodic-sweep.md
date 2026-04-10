---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0235: Periodic SIM sweep for missed-forward recovery

## Motivation

`sweepExistingSms()` is called only at:
1. Boot (end of `setup()`)
2. WiFi-down recovery (transport check in `loop()`)

Scenario: Telegram's API is temporarily unreachable (server restart, load
balancer hiccup) while WiFi stays up. An SMS arrives during the outage:
1. +CMTI fires → `handleSmsIndex()` → `sendMessageReturningId()` fails
2. SMS stays on SIM
3. Telegram API recovers
4. NO sweep is triggered — WiFi was never "down", so the transport
   check never calls `sweepExistingSms()`
5. SMS is stranded on SIM until next reboot

## Plan

Add a periodic `sweepExistingSms()` call every 30 minutes in `loop()`.
Only fire when `activeTransport != kNone` (transport is up).
Gate with `esp_task_wdt_reset()` before and after — the sweep may
process several SMS and take a few seconds.

30 minutes balances: fast enough to recover from transient Telegram
outages within half an hour, slow enough to not stress the modem with
frequent `AT+CMGL` scans.

## Notes for handover

The sweep sends `AT+CMGL=4` to list all stored SMS, then processes each.
If the SIM is empty (common case), it returns in < 100 ms. The 30-minute
interval means the WDT (120s) is never at risk from this periodic call.
