---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0049: Reject `/send` immediately when body exceeds 10-part limit

## Motivation

If a user sends a message body that exceeds the 10-part cap (~1530 GSM-7 /
~670 Unicode chars), the entry is enqueued, `SmsSender` calls
`buildSmsSubmitPduMulti` which returns an empty vector, and the send fails.
After 5 retry attempts the user finally gets "❌ SMS to X failed after
retries." — 30+ seconds after the command, with no explanation why.

Checking the part count before enqueueing gives an immediate, clear error.

## Plan

**`src/telegram_poller.cpp`** — in the `/send` handler, before `enqueue()`:
```cpp
int parts = sms_codec::countSmsParts(body);
if (parts == 0) {
    sendErrorReply(u.chatId, "Message too long (max ~1530 GSM-7 / ~670 Unicode chars).");
    return;
}
```

The `parts` variable is also reused for the RFC-0037 part-count display,
eliminating a redundant `countSmsParts()` call.

## Notes for handover

Only `src/telegram_poller.cpp` changed. No test changes needed — the
rejection path uses existing infrastructure and `countSmsParts` is already
fully tested.
