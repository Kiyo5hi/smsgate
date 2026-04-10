---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0111 — Outbound SMS deduplication

## Motivation

If the user sends `/send +8613800138000 Hello` twice in quick succession (or
if a Telegram retry delivers the command update twice), the same SMS is
enqueued and sent twice. The recipient receives two identical messages.
A simple dedup check on enqueue prevents this: if an identical `(phone, body)`
pair is already in the pending queue, reject the second enqueue and return a
distinct sentinel so the caller can detect the duplicate.

## Plan

1. In `SmsSender::enqueue`, before adding a new entry, iterate the queue and
   check for an existing occupied entry with the same `phone` and `body`. If
   found, return `false` without modifying the queue. The caller (TelegramPoller)
   can then reply "already queued" or ignore the duplicate silently.

2. Change `SmsSender::enqueue` return type from `void` to `bool`.
   `true` = new entry added; `false` = duplicate detected (not added).

3. In `TelegramPoller`, check the return value of `smsSender_.enqueue(...)`.
   If false, send an error reply: `"⚠️ Already queued to <phone>."`.

4. The dedup window is the full queue — as long as an entry for the same
   (phone, body) is pending (not yet delivered or failed), duplicates are
   rejected. Once an entry is drained (success or final failure), the slot is
   freed and a new identical entry can be enqueued.

5. Update `SmsSender::enqueue` declaration and all call sites.

6. Tests for the dedup path.

## Notes for handover

This only protects against accidental double-sends where the body is
identical. Deliberate re-sends (same number, different body) or re-sends
after the first delivery are not affected.

The `/sendall` handler enqueues N entries. Each enqueue call independently
checks dedup, so `/sendall` won't double-enqueue within a broadcast, but if
`/sendall` is invoked twice in quick succession, the second invocation would
be rejected per-alias. This is the desired behavior.
