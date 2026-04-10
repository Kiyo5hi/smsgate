---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0182: Persist gmtOffsetMinutes and fwdTag to NVS

## Motivation

`/setgmtoffset`, `/setgmtoffsetmin`, and `/setfwdtag` are runtime-only
settings: they take effect immediately but are silently discarded on
reboot. A user who sets their timezone to UTC+5:30 or adds a custom
forward tag is surprised to find the defaults restored after a watchdog
reset or firmware update.

`hb_interval`, device label, and auto-reply text already survive reboots
via NVS. Timezone and fwdTag should too.

## Plan

### NVS keys

| Key          | Type        | Default | Notes                          |
|--------------|-------------|---------|--------------------------------|
| `gmt_off_min`| int32_t     | 480     | Total minutes offset (-720..840)|
| `fwd_tag`    | char[21]    | ""      | Up to 20 chars + NUL           |

### Load at boot (in the NVS success block in setup())

```cpp
// RFC-0182: Restore timezone and fwdTag.
int32_t gmtOffMin = 480;
if (realPersist.loadBlob("gmt_off_min", &gmtOffMin, sizeof(gmtOffMin)) == sizeof(gmtOffMin))
    smsHandler.setGmtOffsetMinutes(gmtOffMin);

char fwdTagBuf[21] = {};
if (realPersist.loadBlob("fwd_tag", fwdTagBuf, sizeof(fwdTagBuf)) > 0)
    smsHandler.setFwdTag(String(fwdTagBuf));
```

### Save on mutation

In the `setGmtOffsetFn` lambda (covers both /setgmtoffset and /setgmtoffsetmin):
```cpp
smsHandler.setGmtOffsetMinutes(m);
int32_t v = m;
realPersist.saveBlob("gmt_off_min", &v, sizeof(v));
```

In the `setFwdTagFn` lambda (covers /setfwdtag and /clearfwdtag, which passes ""):
```cpp
smsHandler.setFwdTag(tag);
char buf[21] = {};
tag.toCharArray(buf, sizeof(buf));
realPersist.saveBlob("fwd_tag", buf, sizeof(buf));
```

## Notes for handover

- No new commands — the existing `/setgmtoffset`, `/setgmtoffsetmin`,
  `/setfwdtag`, and `/clearfwdtag` commands automatically persist now.
- `/settings` and `/smshandlerinfo` already display the current values.
- NVS slot usage increases by 2 entries.
- Flash usage change: negligible (just two NVS calls added to lambdas).
