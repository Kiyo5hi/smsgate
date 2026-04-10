---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0126 — /ip command

## Motivation

The operator needs a quick view of the device's WiFi connectivity:
IP address, SSID, and RSSI — without the full `/status` dump.

## Plan

1. Add `setIpFn(std::function<String()> fn)` setter to `TelegramPoller`.
   When set, `/ip` calls this fn and replies with the result. When not
   set, replies "(ip info not configured)".

2. In `main.cpp`, wire a lambda:
   ```
   🌐 192.168.1.42 | SSID: MyWiFi | RSSI: -65 dBm
   ```

3. Tests:
   - `/ip` with fn set → replies with fn result.
   - `/ip` without fn → replies "(ip info not configured)".
