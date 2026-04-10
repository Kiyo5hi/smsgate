---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0061: Duplicate SMS suppression

## Motivation

Some carriers retransmit an SMS when they don't receive an acknowledgement
quickly enough. Without dedup, the user receives the same Telegram message
twice (or more). A 30-second window captures the typical carrier retry
interval.

## Plan

**`src/sms_handler.h`**:
- Public constants `kDedupWindowMs = 30000` and `kDedupSlots = 8`.
- Private `DedupEntry` ring buffer (hash + timestamp).
- Private `checkDup(sender, body) const` — read-only ring scan.
- Private `recordDedup(sender, body)` — write to ring; called only after a
  successful Telegram send so that failed attempts never consume a slot and
  retries still reach the bot.
- `insertFragmentAndMaybePost` gains optional `bool *pWasDuplicate` out-param.

**`src/sms_handler.cpp`**:
- `dedupHash`: djb2 over `sender + '\0' + body`; never returns 0.
- Single-part path: `checkDup` before the bot call; `recordDedup` after
  successful send; sets `logEntry.outcome = "dup"` and deletes the SIM slot.
- Concat assembled path: same pattern inside `insertFragmentAndMaybePost`.

## Notes for handover

Changed: `src/sms_handler.{h,cpp}`, `test/test_native/test_sms_handler.cpp`
(3 new dedup tests; 3 pre-existing tests updated to use distinct bodies so
they don't trigger the new dedup logic).
