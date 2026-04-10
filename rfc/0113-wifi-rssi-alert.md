---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0113 — WiFi low-RSSI proactive alert

## Motivation

A poor WiFi signal (-80 dBm or worse) can cause intermittent TLS
failures and slow Telegram delivery even while the device reports
`WL_CONNECTED`. The operator only learns about this when messages
start failing; by then, multiple SMS may have been delayed or dropped.
A proactive alert lets the operator reposition the device before
problems occur.

## Plan

1. Add `static bool s_lowWifiRssiAlertSent = false;` to `main.cpp`.

2. In the existing 30-second modem-cache refresh block in `loop()`,
   after the network-registration alert (RFC-0082), add:

   ```cpp
   // RFC-0113: WiFi low-RSSI alert.
   if (WiFi.status() == WL_CONNECTED) {
       int rssi = WiFi.RSSI();
       if (rssi < -80 && !s_lowWifiRssiAlertSent && !alertsMuted()) {
           realBot.sendMessage("⚠️ Weak WiFi: " + String(rssi) + " dBm. "
               "Telegram delivery may be unreliable.");
           s_lowWifiRssiAlertSent = true;
       } else if (rssi >= -70) {
           s_lowWifiRssiAlertSent = false; // hysteresis: clear when signal recovers
       }
   } else {
       s_lowWifiRssiAlertSent = false; // reset on disconnect so alert re-fires on reconnect
   }
   ```

   Thresholds: alert at < −80 dBm, clear at ≥ −70 dBm (10 dBm of
   hysteresis prevents oscillation near the boundary).

3. No new tests: this is `main.cpp`-only glue, not testable from the
   native env (depends on `WiFi.RSSI()` and `realBot` globals).

## Notes for handover

This follows the exact same pattern as the heap-low alert (RFC-0066)
and the network-registration alert (RFC-0082). `s_lowWifiRssiAlertSent`
is reset when the device is disconnected, so the alert re-fires after
every reconnect if the signal is still poor.
