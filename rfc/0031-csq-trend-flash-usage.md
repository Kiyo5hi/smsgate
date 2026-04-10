---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0031: CSQ trend ring buffer + flash usage in `/status`

## Motivation

A single CSQ reading in `/status` can be misleading — a momentary good reading
masks an unstable connection. A 6-sample trend (one per 30 s = 3-minute window)
shows whether signal quality is stable, improving, or degrading.

Additionally, flash usage (sketch size / total) is useful for headroom monitoring
when adding features.

## Plan

### CSQ trend

Add three file-statics in `main.cpp`:

```cpp
static int8_t  csqHistory[6]   = {};
static uint8_t csqHistoryIdx   = 0;
static bool    csqHistoryFull  = false;
```

In the 30-second refresh block (alongside `AT+COPS?` and `modem.getSignalQuality()`):

```cpp
csqHistory[csqHistoryIdx] = cachedCsq;
csqHistoryIdx = (csqHistoryIdx + 1) % 6;
if (csqHistoryIdx == 0) csqHistoryFull = true;
```

In `statusFn`, append the history after the CSQ value:

```
Modem: CSQ 18 (good)  home (China Unicom) [15 13 17 19 18 18]
```

Only show entries that have been populated (use `csqHistoryFull` + `csqHistoryIdx`).
Show up to 6 values in chronological order (oldest → newest).

### Flash usage

```cpp
uint32_t sketchSize  = ESP.getSketchSize();
uint32_t sketchTotal = ESP.getFreeSketchSpace() + sketchSize;
line += "Flash: " + String(sketchSize/1024) + "kB / " + String(sketchTotal/1024) + "kB\n";
```

## Notes for handover

Only `src/main.cpp` changed. No test changes needed (statusFn is composed entirely
in main.cpp, not a testable unit).
