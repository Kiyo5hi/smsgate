---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0193: `/sendnow` — immediately fire all scheduled SMS

## Motivation

When a scheduled SMS was set with `/schedulesend` but the operator wants
to send it right away (e.g. timing changed), they currently have to cancel
(`/cancelsched`) and re-send manually. `/sendnow` collapses the wait.

## Plan

### TelegramPoller: `/sendnow`

Iterates over `scheduledQueue_`, sets `sendAtMs = 1` (non-zero so it
fires, but less than any realistic `clock_()` value since the first tick
will see `now >= 1`) on every occupied slot.

Actually cleaner: set `sendAtMs = clock_()` so the drain on the very
next tick fires all of them.

- If queue is empty, replies "(no scheduled SMS to send)"
- If N > 0 entries, sets them all to fire now and replies
  "✅ Triggered N scheduled SMS."

No setter needed — all state is in TelegramPoller itself.
No NVS persistence.

## Notes for handover

- This fires all slots indiscriminately. There's no per-slot sendnow.
  Use `/cancelsched` to cancel individual entries.
