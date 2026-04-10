---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0046: /cancel <N> command to remove a queued outbound SMS

## Motivation

`/queue` shows pending outbound SMS entries (numbered 1, 2, 3...). If a
message is stuck retrying after a modem error the user previously had no way
to remove it without rebooting. `/cancel N` removes entry N immediately.

## Plan

**`src/sms_sender.h`** — add method:
```cpp
bool cancelQueueEntry(int n); // 1-indexed, returns false if out of range
```

**`src/sms_sender.cpp`** — iterate occupied slots, remove the Nth one.
`onFinalFailure` is NOT called — cancellation is intentional, not exhaustion.

**`src/telegram_poller.cpp`** — add handler after `/queue`:
```cpp
if (lower == "/cancel" || lower.startsWith("/cancel ")) {
    int n = extractArg(lower, "/cancel ").toInt();
    if (smsSender_.cancelQueueEntry(n))
        bot_.sendMessageTo(u.chatId, "✅ Queue entry N cancelled.");
    else
        sendErrorReply(u.chatId, "No entry N in queue (use /queue to list).");
}
```

**`src/telegram.cpp`** — register `/cancel` command.

## Notes for handover

Tests in `test/test_native/test_sms_sender.cpp`:
`test_SmsSender_cancel_removes_entry`, `test_SmsSender_cancel_out_of_range_returns_false`.
