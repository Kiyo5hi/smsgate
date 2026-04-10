---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0050: Reject reply-path messages that exceed 10-part limit

## Motivation

RFC-0049 added the too-long check to `/send`. The same silent failure could
occur when a user sends a very long Telegram reply to a forwarded SMS — the
reply would be enqueued, fail 5 times, and surface as "failed after retries"
with no explanation. Apply the same guard to the reply path.

## Plan

**`src/telegram_poller.cpp`** — in the reply-path (after the empty-body
check), add:
```cpp
if (sms_codec::countSmsParts(u.text) == 0) {
    sendErrorReply(u.chatId, "Reply too long (max ~1530 GSM-7 / ~670 Unicode chars).");
    return;
}
```

## Notes for handover

Only `src/telegram_poller.cpp` changed. No test changes needed.
