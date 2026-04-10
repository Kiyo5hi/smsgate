---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0056: Periodic NTP resync every 24 hours

## Motivation

The ESP32's RTC accumulates drift over days. For a bridge running continuously
for weeks or months the timestamp shown in `/status` and SMS forwarding headers
can become inaccurate. Resyncing once per day keeps the clock within a second
of wall time without the user having to issue `/ntp` manually.

## Plan

**`src/main.cpp`** — add after the heartbeat block:
```cpp
static uint32_t lastNtpResyncMs = 0;
constexpr uint32_t kNtpResyncIntervalMs = 24UL * 60UL * 60UL * 1000UL;
if (activeTransport == kWiFi &&
    (nowMs - lastNtpResyncMs) >= kNtpResyncIntervalMs &&
    lastNtpResyncMs != 0)  // skip first iteration (boot already synced)
{
    lastNtpResyncMs = nowMs;
    esp_task_wdt_reset();
    syncTime();
}
else if (lastNtpResyncMs == 0)
    lastNtpResyncMs = nowMs; // initialise
```

Only runs on the WiFi path. No notification is sent (boot banner already
shows the synced time; the next `/status` will reflect the updated clock).

## Notes for handover

Only `src/main.cpp` changed. No test changes needed.
