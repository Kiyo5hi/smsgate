---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0047: Sweep existing SMS after WiFi reconnect

## Motivation

When WiFi drops, `handleSmsIndex` is still called for each `+CMTI` URC but
the Telegram forward fails (Telegram unreachable). The SMS stays on the SIM.
On reconnect the bridge sends a "🔗 WiFi reconnected" notification but does
not retry those stuck messages — they sit on the SIM until the next reboot.
Triggering `sweepExistingSms()` immediately after reconnect forwards them
without requiring a manual restart.

## Plan

**`src/main.cpp`** — in the WiFi reconnect branch (RFC-0039), after sending
the reconnect notification:
```cpp
esp_task_wdt_reset();   // sweep can take several AT round-trips; pet WDT
smsHandler.sweepExistingSms();
```

## Notes for handover

Only `src/main.cpp` changed (WiFi reconnect block). No test changes needed.
`sweepExistingSms` is already well-exercised in `test_sms_handler.cpp`.
The WDT reset guards against the ~1s per SIM slot overhead if the SIM is full
of stuck messages.
