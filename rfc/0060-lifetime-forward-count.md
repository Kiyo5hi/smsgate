---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0060: Lifetime SMS forward count in NVS

## Motivation

`smsHandler.smsForwarded()` resets on every reboot, making it impossible to
know the total number of SMS the bridge has ever forwarded. A lifetime counter
persisted to NVS survives reboots and gives a useful long-term metric.

## Plan

**`src/main.cpp`**:
- File-scope static `static uint32_t s_lifetimeFwdCount = 0;`
- At transport-ready init: load from NVS key `"lifetimefwd"`.
- In `setOnForwarded` lambda: increment + `saveBlob("lifetimefwd", ...)`.
- In `statusFn`: `"Forwarded: N (session), M (lifetime)"`.

## Notes for handover

Only `src/main.cpp` changed. No test changes needed (NVS path is
hardware-only and the callback mechanism is already tested via RFC-0041).
