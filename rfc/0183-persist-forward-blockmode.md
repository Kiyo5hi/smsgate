---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0183: Persist forwardingEnabled and blockingEnabled to NVS

## Motivation

`/setforward off` and `/setblockmode on` are operator actions with
real security/operational consequences that should survive a reboot.
A device rebooted after a watchdog reset silently re-enables SMS
forwarding or disables the block list — the exact opposite of what
the operator configured. RFC-0182 established the pattern for
persisting runtime settings; this RFC applies it to two more critical
flags.

## Plan

### NVS keys

| Key          | Type    | Default | Notes                              |
|--------------|---------|---------|-------------------------------------|
| `fwd_enabled`| uint8_t | 1       | 1=ON, 0=OFF; matches bool semantics |
| `blk_enabled`| uint8_t | 1       | 1=ON, 0=OFF                         |

### Load at boot (in the NVS success block in setup())

```cpp
// RFC-0183: Restore forwarding and block-mode flags.
{
    uint8_t v = 1;
    if (realPersist.loadBlob("fwd_enabled", &v, sizeof(v)) == sizeof(v))
        smsHandler.setForwardingEnabled(v != 0);
}
{
    uint8_t v = 1;
    if (realPersist.loadBlob("blk_enabled", &v, sizeof(v)) == sizeof(v))
        smsHandler.setBlockingEnabled(v != 0);
}
```

### Save on mutation

In `setForwardingEnabledFn` lambda:
```cpp
smsHandler.setForwardingEnabled(enabled);
uint8_t v = enabled ? 1 : 0;
realPersist.saveBlob("fwd_enabled", &v, sizeof(v));
```

In `setBlockingEnabledFn` lambda:
```cpp
smsHandler.setBlockingEnabled(enable);
uint8_t v = enable ? 1 : 0;
realPersist.saveBlob("blk_enabled", &v, sizeof(v));
```

## Notes for handover

- No new commands; `/setforward` and `/setblockmode` automatically persist.
- Default is ON for both — on first boot with no NVS key, the current
  default behaviour is preserved.
- NVS slot usage increases by 2 entries.
