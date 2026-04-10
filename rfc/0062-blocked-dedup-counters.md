---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0062: Blocked and dedup counters in /status

## Motivation

After RFC-0018/0021 (block lists) and RFC-0061 (dedup), `/status` had no way
to confirm these features are actually firing. Two session counters
(`smsBlocked_`, `smsDeduplicated_`) make it visible.

## Plan

**`src/sms_handler.h`**:
- Add `int smsBlocked_ = 0;` and `int smsDeduplicated_ = 0;` members.
- Add `int smsBlocked() const` and `int smsDeduplicated() const` accessors.

**`src/sms_handler.cpp`**:
- Increment `smsBlocked_` in the block-list path.
- Increment `smsDeduplicated_` in both single-part and concat dedup paths.

**`src/main.cpp`** `statusFn`:
- Append `"  Blocked: N\n"` and `"  Deduped: N\n"` (only when > 0) after the
  Forwarded line in the 📨 SMS section.

## Notes for handover

Changed: `src/sms_handler.{h,cpp}`, `src/main.cpp`. No test changes needed
(existing block-list tests implicitly cover the increment path).
