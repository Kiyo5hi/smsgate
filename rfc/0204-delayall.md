---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0204: `/delayall <minutes>` — extend all scheduled slots at once

## Motivation

`/scheddelay <N> <min>` delays one slot. When you want to push back all
pending messages (e.g. you scheduled 3 messages but realize you need 30
more minutes), you'd have to run `/scheddelay` 3 times. `/delayall` does
it in one command.

## Design

Command: `/delayall <min>` where `min` is 1–1440.

Behavior:
- Extend every occupied slot's `sendAtMs` by `min * 60000`.
- Reply: `"⏰ Extended 3 slot(s) by 30 min."` or
  `"(no scheduled SMS to delay)"` if queue is empty.
- Call `persistSchedFn_()` after mutation.

Add to `/help` and implement in `telegram_poller.cpp`.

## Notes for handover

- Range validation: min must be 1–1440 (same as `/schedulesend`).
- If a slot's new sendAtMs would overflow uint32_t, cap at UINT32_MAX.
  In practice this is ~49 days of millis() which is well past any
  reasonable delay, so cap is a safety guard only.
