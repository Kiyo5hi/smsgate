---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0189: Persist maxParts, concatTtlSec, maxFail, dedupWindowMs to NVS

## Motivation

Completes the NVS persistence sweep. Four remaining runtime-configurable
settings are lost on reboot:

| Command          | Setting             | Default |
|-----------------|---------------------|---------|
| `/setmaxparts`  | maxParts            | 10      |
| `/setconcatttl` | concatTtlSec        | 86400   |
| `/setmaxfail`   | maxConsecutiveFail  | 8       |
| `/setdedup`     | dedupWindowMs/secs  | 0       |

## NVS keys

| Key           | Type     | Default |
|---------------|----------|---------|
| `max_parts`   | int32_t  | 10      |
| `concat_ttl`  | uint32_t | 86400   |
| `max_fail`    | int32_t  | 8       |
| `dedup_secs`  | uint32_t | 0       |

## Load at boot

After the existing NVS-success block loads (after RFC-0185):
```cpp
{ int32_t v = 10;    if (loadBlob("max_parts", ...)  == sz) smsSender.setMaxParts(v); }
{ uint32_t v = 86400; if (loadBlob("concat_ttl", ...) == sz) smsHandler.setConcatTtlMs(v*1000); }
{ int32_t v = 8;    if (loadBlob("max_fail", ...)   == sz) smsHandler.setMaxConsecutiveFailures(v); }
{ uint32_t v = 0;   if (loadBlob("dedup_secs", ...) == sz) smsHandler.setDedupWindowMs(v*1000); }
```

## Save on mutation

In each setter lambda, call `realPersist.saveBlob(key, &v, sizeof(v))`.

## Notes for handover

- No new commands needed.
- NVS slot usage increases by 4.
- Flash delta: negligible (a few NVS calls).
