---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0252: WDT kicks in the 30s status refresh block

## Motivation

The 30-second status refresh block in `loop()` can call
`realBot.sendMessage()` up to 8 times in one execution — one for each
alert type that might fire on the same tick:

| Alert | RFC |
|---|---|
| Network registration lost / restored | RFC-0082 |
| Weak WiFi RSSI | RFC-0113 |
| SIM storage near-full | RFC-0064 |
| Low CSQ signal | RFC-0081 |
| Critical heap (before reboot) | RFC-0073 |
| Low heap | RFC-0066 |
| Stuck outbound queue | RFC-0096 |
| 24-hour stats digest | RFC-0075 |

Each `sendMessage()` call can block up to ~23 s (15 s connect timeout +
4 s header + 4 s body drain, all bounded by RFC-0231/RFC-0233).
8 × 23 s = 184 s, which exceeds the 120 s WDT without a single
intervening `esp_task_wdt_reset()`. In a catastrophic scenario where
all conditions are simultaneously bad (network degraded, Telegram slow),
the device would reboot from the watchdog while trying to report the
problem — exactly the worst time to be resetting.

The RFC-0079 NTP retry also lives in this block and calls `syncTime()`
which has its own internal WDT kicks (RFC-0249), but the other alert
sends were unprotected.

## Plan

1. Add `esp_task_wdt_reset()` at the **entry** of the 30s block (right
   after `lastStatusRefreshMs = millis()`) so the entire block starts
   with a fresh 120 s window.

2. Add `esp_task_wdt_reset()` immediately before **each**
   `realBot.sendMessage()` call in the block. This ensures a fresh
   window regardless of how many prior sends already consumed time.

All 9 send sites (8 alerts + NTP clock-synced notification) are covered.

## Notes for handover

`main.cpp` always compiles for ESP32 (never for the native test env),
so no `#ifdef ESP_PLATFORM` guard is needed at these call sites.
The native test env never includes `main.cpp` via
`build_src_filter = ... -<main.cpp>`.

The total worst-case duration of the block is still bounded by 8 × 23 s +
AT command overhead + syncTime = ~200 s, but each of the 9 WDT kicks
resets the 120 s clock, so no single gap exceeds ~23 s.
