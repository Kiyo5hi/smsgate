---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0237: WiFi reconnect escalation after 5 minutes down

## Motivation

When WiFi drops while `activeTransport == kWiFi`, the transport-check block
calls `WiFi.reconnect()` every 30 s. On some ESP32 WiFi driver versions,
`WiFi.reconnect()` gets stuck internally and will not recover without a full
`WiFi.disconnect() + WiFi.begin(ssid, password)` cycle. Without escalation,
the device can be offline for hours with the driver spinning silently until
the next WDT-triggered reboot (which never comes because `esp_task_wdt_reset`
is called every loop).

## Plan

Track `wifiDownSinceMs` (already set in RFC-0039). Add a second threshold:
`kWifiHardResetMs = 5 * 60 * 1000` (5 min). When WiFi has been continuously
down for more than 5 min, call `WiFi.disconnect(false) + WiFi.begin(ssid, password)`
instead of just `WiFi.reconnect()`. The call is non-blocking; we check the
result on the next transport-check tick (30 s later).

Only fires when `activeTransport == kWiFi` (not kCellular — the cellular path
already has its own non-blocking WiFi recovery).

## Notes for handover

`wifiDownLastCheck` is set on the first consecutive missed check (i.e. two
consecutive 30s ticks with WiFi down). `wifiDownSinceMs` is set at the same
time. The escalation fires when `(millis() - wifiDownSinceMs) > kWifiHardResetMs`
AND we're already in the `wifiDownLastCheck == true` state.
