---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0265: /tschedule missing upper-bound validation on delay minutes

## Motivation

`/tschedule <name> <min> <phone>` validates `delayMin <= 0` but does NOT
enforce a maximum.  The corresponding calculation:

```cpp
uint32_t sendAt = nowMs + (uint32_t)delayMin * 60000U;
```

overflows `uint32_t` when `delayMin > 71582` (≈ 49.7 days), because
`71583 × 60 000 = 4 294 980 000 > UINT32_MAX`.  The overflow wraps `sendAt`
to a value close to `nowMs`, so the scheduled SMS fires almost immediately
instead of at the intended far-future time.

All sibling commands that accept a minute argument cap the value:

| Command          | Cap       |
|------------------|-----------|
| `/schedulesend`  | 1 – 1440  |
| `/schedclone`    | 1 – 1440  |
| `/scheddelay`    | 1 – 1440  |
| `/delayall`      | 1 – 1440  |
| `/pausefwd`      | 1 – 1440  |
| `/mute`          | 1 – 1440  |
| `/snooze`        | 1 – 480   |
| `/recurring`     | 1 – 10080 |
| **`/tschedule`** | **none ← bug** |

## Plan

Replace:
```cpp
if (delayMin <= 0)
{
    bot_.sendMessageTo(u.chatId, String("Delay must be a positive integer (minutes)."));
    return;
}
```
with:
```cpp
if (delayMin < 1 || delayMin > 1440)
{
    bot_.sendMessageTo(u.chatId,
        String("\xe2\x9d\x8c Delay must be 1\xe2\x80\x93 1440 minutes.")); // ❌ –
    return;
}
```

Same range as `/schedulesend` (24 h max), consistent error message style.

## Notes for handover

- No state change; no NVS schema change.
- The fix is a one-line condition change.
