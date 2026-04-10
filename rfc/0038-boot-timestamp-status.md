---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0038: Boot timestamp in `/status`

## Motivation

`/status` shows uptime (relative) but not when the device actually booted.
If the uptime is "2d 3h", knowing the absolute boot time is more useful for
correlating with events (when did it restart? was it before or after the network
change?). With NTP already synced at boot, this is free.

## Plan

**`src/main.cpp`**

Add `static time_t s_bootTimestamp = 0;` file-scope static.

In `syncTime()`, after the loop completes:
```cpp
if (s_bootTimestamp == 0)
    s_bootTimestamp = now;
```

In `statusFn` Device section, after the Reset line:
```cpp
if (s_bootTimestamp > 0) {
    time_t bt = s_bootTimestamp + TIMEZONE_OFFSET_SEC;
    strftime(bootBuf, sizeof(bootBuf), "%Y-%m-%d %H:%M", gmtime(&bt));
    msg += "  Booted: " + bootBuf + " " + tzLabel + "\n";
}
```

Result: `Booted: 2026-04-10 14:32 UTC+8`

## Notes for handover

Only `src/main.cpp` changed. No test changes needed.
