---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0214: `/queueinfo <N>` — detailed view of outbound queue entry

## Motivation

`/queue` shows a compact list of pending outbound SMS entries but
truncates the body preview to ~20 chars and doesn't show the retry
count or next-retry ETA. Operators debugging a stuck outbound SMS
need the full body and retry timing.

## Design

New command: `/queueinfo <N>` (1-based, matching `/queue` display order)

Output:
```
📤 Queue slot 2/8
Phone:    +13800138000
Body:     Full message body text here...
Attempts: 2 / 5
Queued:   42.3s ago
Next retry: in 18s
```

Uses `SmsSender::getQueueSnapshot()` for the data. The existing
`QueueSnapshot` struct already carries phone, bodyPreview, attempts,
and queuedAtMs; we need the full body too — but bodyPreview is the
first ≤20 chars. To show the full body we'd need a new method or
to extend QueueSnapshot.

Simpler approach: add `SmsSender::getFullBody(int idx)` (0-based)
that returns the full body for occupied slot idx, or empty string if
free/out-of-range. Then `/queueinfo N` uses getQueueSnapshot() for
phone+attempts+timing and getFullBody(idx) for the full body.

## Notes for handover

- `SmsSender::getFullBody(int idx)` — new public method on SmsSender
  (not on ISmsSender; used only by TelegramPoller command path).
- 1-based slot index in the command; passed as (n-1) to getFullBody.
- "Next retry: in Xs" — computed from `(nextRetryMs - nowMs) / 1000`
  if nextRetryMs > nowMs, else "now".
- "Queued: Xs ago" — computed from `(nowMs - queuedAtMs) / 1000`
  if queuedAtMs > 0.
- No new NVS, no new interface changes.
