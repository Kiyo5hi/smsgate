---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0039: WiFi reconnect Telegram notification

## Motivation

When WiFi drops briefly (e.g. router restart, power blip) and recovers, the bridge
comes back silently. The user has no way to know there was a connectivity gap without
checking uptime or the serial log. A "🔗 WiFi reconnected (was down 42s)" notification
lets the user know how long the bridge was unreachable for SMS/Telegram traffic.

## Plan

**`src/main.cpp`** — in the existing 30s transport-check block:

Add `static unsigned long wifiDownSinceMs = 0;` alongside `wifiDownLastCheck`.

When WiFi is detected as down (setting `wifiDownLastCheck = true`), also record
`wifiDownSinceMs = millis()`.

In the `else` branch (WiFi is connected), when `wifiDownLastCheck` was true:
```cpp
unsigned long downSec = (millis() - wifiDownSinceMs) / 1000UL;
String notif = "🔗 WiFi reconnected (was down ";
if (downSec >= 60) notif += String(downSec/60) + "m ";
notif += String(downSec%60) + "s)";
realBot.sendMessage(notif);
```

## Notes for handover

Only `src/main.cpp` changed. No test changes needed (transport logic uses WiFi
hardware state not injectable in native tests). Notification only fires for the
WiFi-primary path (`activeTransport == kWiFi`); cellular path is not affected.
