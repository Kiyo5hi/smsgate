---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0261: Extend scheduled-SMS body limit from 127 → 1530 chars

## Motivation

The NVS persistence blob for the scheduled SMS queue (RFC-0200) uses
`char body[128]` per slot — capping stored bodies at 127 chars.

Four scheduler commands (`/scheduleat`, `/sendafter`, `/recurring`,
`/schedrename`) silently truncated bodies to 127 chars at input time,
matching the NVS limit, but gave the user no indication that truncation
was happening.

`/schedulesend` had **no body-length check at all** — it stored the full
body in RAM, but `persistSchedFn_()` silently truncated it to 127 chars
on NVS write. A scheduled SMS could therefore behave differently
depending on whether a reboot happened before the send time:
- **No reboot**: full body delivered ✓
- **Reboot before send**: truncated body delivered ✗

This non-determinism is a data-integrity bug.

## Plan

1. Increase the NVS body field to `char body[1531]` (1530 chars + NUL),
   matching the GSM-7 10-part concat maximum.
2. Bump the blob version to `0x03` so old blobs are discarded on load.
3. Change all five scheduler commands to **reject** bodies > 1530 chars
   with an explicit error message instead of silently truncating.

### New slot layout (v0x03)

| Field | Size |
|-------|------|
| sendAtUnix (uint32_t) | 4 B |
| phone (char[32]) | 32 B |
| body (char[1531]) | 1531 B |
| repeatIntervalSec (uint32_t) | 4 B |
| **Slot total** | **1571 B** |
| Blob total (1 + 5 × 1571) | **7856 B** |

7.9 KB fits within the NVS partition. Current other blobs:
- `smslog`: 1684 B
- `reply_targets`: 5408 B
- `update_id` etc.: < 100 B
- Total including this blob: ~15.1 KB < 24 KB NVS limit.

### Error message

```
❌ Message too long for scheduled queue (max 1530 chars). Use /send for
   long messages, or shorten to schedule.
```

## Notes for handover

- Old v0x02 blobs are silently discarded on load; users lose any
  previously scheduled SMS but no crash or corruption occurs.
- The 1530-char cap covers GSM-7 only. For UCS-2 bodies the actual
  per-send cap is ~670 chars; over-limit UCS-2 messages are caught at
  send time by SmsSender::send() rather than at schedule time. This is
  acceptable: the scheduler accepts 1530 bytes as the conservative outer
  bound, matching the character count of a max-length GSM-7 message.
