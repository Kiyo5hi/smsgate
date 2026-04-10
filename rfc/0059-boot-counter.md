---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0059: Persistent boot counter

## Motivation

The `/status` command already shows the last reset reason, but not how many
times the device has rebooted over its lifetime. A cumulative boot count in NVS
makes it easy to spot instability (e.g. watchdog loops) at a glance.

## Plan

**`src/main.cpp`** — file-scope static `static uint32_t s_bootCount = 0;`

At the top of `setup()`, after `s_resetReason`:
```cpp
{
    uint32_t bc = 0;
    realPersist.loadBlob("bootcnt", &bc, sizeof(bc));
    bc++;
    realPersist.saveBlob("bootcnt", &bc, sizeof(bc));
    s_bootCount = bc;
}
```

In `statusFn`, append after the `Reset:` line:
```
  Boots: N (lifetime)
```

## Notes for handover

Only `src/main.cpp` changed. No test changes needed (NVS path is
hardware-only).
