---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0136 — /cancelnum command

## Motivation

`/cancel <N>` cancels a specific queued SMS by index. When the operator
wants to cancel all pending SMS to a specific phone number (e.g. before
swapping a SIM), they'd have to find and cancel each entry by index.
`/cancelnum <phone>` cancels all queued entries for a given number.

## Plan

1. Add `cancelByPhone(phone)` method to `SmsSender` that removes all
   queue entries with the given phone number and returns the count removed.

2. Wire `/cancelnum <phone>` in `TelegramPoller`. When the queue has
   entries for that number, removes them and replies
   "✅ Cancelled N entries for <phone>." When none found, replies
   "(no queued entries for <phone>)". When arg missing, replies usage.

3. Tests:
   - `/cancelnum <phone>` with matching entries → removes and confirms.
   - `/cancelnum <phone>` with no matching entries → placeholder.
   - `/cancelnum` with no arg → usage error.
