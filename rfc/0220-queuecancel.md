---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0220: /queuecancel <N> — cancel a queued SMS

## Motivation

The operator can inspect (`/queueinfo`), clone (`/schedclone`), and retry (`/retry`) queue
entries, but has no way to remove one. If a message was queued by mistake or the recipient
is no longer reachable, the only escape is waiting for all retries to exhaust. Adding
`/queuecancel <N>` closes this gap.

## Current state

`SmsSender` has no cancel primitive. The queue is a fixed-size ring buffer of `OutboundEntry`
items; cancellation means marking a slot as free (clearing `phone` and resetting state).

## Plan

1. Add `bool cancelEntry(int n)` to `SmsSender` / `ISmsSender`.
   - `n` is 1-based, matching `/queueinfo` display.
   - Returns `false` if `n` out of range or slot not occupied.
   - Marks the slot as free: set `phone = ""`, `attempts = 0`, `nextRetryMs = 0`.
2. In `TelegramPoller::processUpdate`, add `/queuecancel <N>` handler:
   - Parse N, call `smsSender_.cancelEntry(N)`, reply success or error.
3. Add to `/help`.
4. Tests: cancel valid entry, cancel out-of-range, cancel already-empty slot.

## Notes for handover

Already implemented before this RFC was written:
- `SmsSender::cancelQueueEntry(int n)` in `sms_sender.cpp` (RFC-0046 tag)
- `/cancel <N>` handler in `telegram_poller.cpp` (RFC-0046 tag)
- `/cancelnum <phone>` handler (`SmsSender::cancelByPhone`) in `telegram_poller.cpp` (RFC-0136 tag)
- Both listed in `/help`

No implementation work required.
