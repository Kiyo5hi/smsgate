---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0264: Fix /resetwatermark writing to wrong NVS key

## Motivation

`TelegramPoller::resetWatermark()` (RFC-0152) saves the zeroed watermark to
NVS key `"last_update_id"` via `persist_.saveBlob(...)`.  However,
`TelegramPoller::begin()` loads the watermark via `persist_.loadLastUpdateId()`
which reads key `"uid"`.  These are two different NVS keys in the same
namespace, so the save and load are completely disconnected.

**Consequence:** Calling `/resetwatermark` correctly resets `lastUpdateId_` in
RAM (the current session works), but does NOT persist the reset.  If the device
reboots before the next successful `tick()` saves the new watermark to `"uid"`,
`begin()` loads the old watermark from `"uid"` and the reset is undone.

The key `"last_update_id"` is written but never read — it accumulates stale
data in NVS every time `/resetwatermark` is called.

## Plan

Replace:
```cpp
persist_.saveBlob("last_update_id", &lastUpdateId_, sizeof(lastUpdateId_));
```
with:
```cpp
persist_.saveLastUpdateId(lastUpdateId_);
```

This writes to the same `"uid"` key that `loadLastUpdateId()` reads, making
the reset durable across reboots.

## Notes for handover

- No schema change; no version bump needed.
- The stale `"last_update_id"` key that may exist in devices that ran the
  broken version is harmless — it will simply be ignored forever.
- The fix is a one-line change; no logic change beyond the key name.
