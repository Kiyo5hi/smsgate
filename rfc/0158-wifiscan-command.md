---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0158: /wifiscan command — scan nearby WiFi networks

## Motivation

When the bridge loses WiFi and the user can't tell if the target SSID
is even visible, `/wifiscan` returns a list of nearby SSIDs with RSSI
and channel. Helps distinguish "wrong password" from "AP out of range"
from "channel congestion".

## Plan

Add `setWifiScanFn(std::function<String()>)` to TelegramPoller.
`/wifiscan` calls `wifiScanFn_()` and sends the result.

In main.cpp the lambda calls `WiFi.scanNetworks(false, true)` (blocking,
show hidden), then formats each result as `SSID (ch N, -XX dBm)`.
Returns up to 10 networks sorted by RSSI.
