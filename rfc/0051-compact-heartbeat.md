---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0051: Compact one-line heartbeat message

## Motivation

The heartbeat previously sent the full `/status` output (10+ lines every 6
hours). That's noisy in Telegram — it buries the chat in detail that the user
only wants on demand. A one-liner like
`⏱ 2d 3h 14m | CSQ 18 China Mobile | WiFi -67dBm | fwd 42 | q 0/8`
confirms the bridge is alive and healthy without scrolling.

## Plan

**`src/main.cpp`** — replace the heartbeat block:
```cpp
String hb = "⏱ Xd Xh Xm | CSQ N [operator] | WiFi N dBm | fwd N | q N/8";
realBot.sendMessage(hb);
```

Fields: uptime (days/hours/minutes), CSQ value + operator name (if set),
WiFi RSSI, lifetime SMS forwarded count, current outbound queue depth.

`statusFn` is no longer called from the heartbeat path. Users can still
get the full status via `/status` on demand.

## Notes for handover

Only `src/main.cpp` changed. No test changes needed.
