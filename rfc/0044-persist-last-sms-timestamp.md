---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0044: Persist last SMS received timestamp across reboots

## Motivation

RFC-0041 added the last-SMS timestamp to `/status`, but it reset to zero on
every reboot. After a watchdog-triggered restart the user couldn't tell when
the last SMS had arrived. Persisting the timestamp to NVS makes the field
meaningful even after a cold boot.

## Plan

**`src/main.cpp`**

In the NVS init block (after `smsDebugLog.setSink`), load the stored timestamp:
```cpp
uint32_t ts = 0;
if (realPersist.loadBlob("lastsmsts", &ts, sizeof(ts)) == sizeof(ts) && ts > 0)
    s_lastSmsTimestamp = (time_t)ts;
```

In the `setOnForwarded` lambda, save after updating the in-RAM value:
```cpp
s_lastSmsTimestamp = time(nullptr);
uint32_t ts = (uint32_t)s_lastSmsTimestamp;
realPersist.saveBlob("lastsmsts", &ts, sizeof(ts));
```

NVS key: `"lastsmsts"` (8 chars, within the 15-char limit). Stored as
`uint32_t` (Unix seconds), sufficient until 2106.

## Notes for handover

Only `src/main.cpp` changed. No test changes needed.
