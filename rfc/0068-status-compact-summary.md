---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0068: Compact summary at top of /status

## Motivation

`/status` is several screens of detailed information. Reading it requires
scrolling just to check the basic numbers. Prepending the same compact
one-liner that the heartbeat sends gives the key metrics at a glance.

## Plan

**`src/main.cpp`** `statusFn`:
- Before the "📡 Device" section, insert the heartbeat-format compact line:
  `⏱ Xd Xh Xm | CSQ N [oper] | WiFi NdBm | fwd N | q N/8`
- Separate from the detail section with a blank line.

## Example output

```
⏱ 2d 3h 15m | CSQ 18 China Unicom | WiFi -45dBm | fwd 42 | q 0/8

📡 Device
  Time: 2026-04-10 14:32 UTC+8
  ...
```

## Notes for handover

Only `src/main.cpp` changed.
