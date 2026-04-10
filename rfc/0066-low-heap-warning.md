---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0066: Low heap warning

## Motivation

When free heap drops below ~15 KB string allocations and TLS buffers start
failing silently. A proactive alert before that point helps diagnose memory
leaks or unexpected growth before the device becomes unstable.

## Plan

**`src/main.cpp`**:
- File-scope static `static bool s_lowHeapWarnSent = false;`
- In the 30s refresh block (after the SIM full warning):
  - If `ESP.getFreeHeap() < 15*1024` and not warned: send `"⚠️ Low heap: N B free."`, set flag.
  - If `ESP.getFreeHeap() > 25*1024`: clear flag (hysteresis prevents flapping).

## Notes for handover

Only `src/main.cpp` changed. No test changes needed.
