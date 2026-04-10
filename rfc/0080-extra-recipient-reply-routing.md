---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0080: Reply routing for extra recipients

## Motivation

RFC-0070 fans out forwarded SMS to extra allow-list users using
`sendMessageTo()`, which doesn't return the Telegram message_id. As a
result, extra recipients can see forwarded SMS but their replies are
not reply-routed back to the SMS sender — they get "reply target
not found" errors. For a family/team scenario this is a significant
limitation.

## Plan

Replace `bot_.sendMessageTo(extraRecipients_[i], formatted)` with
`bot_.sendMessageToReturningId(extraRecipients_[i], formatted)` in both
`SmsHandler::forwardSingle` and `SmsHandler::insertFragmentAndMaybePost`.

Store each returned message_id in the reply-target ring:
```cpp
int32_t xmid = bot_.sendMessageToReturningId(extraRecipients_[i], formatted);
if (xmid > 0 && replyTargets_ != nullptr)
    replyTargets_->put(xmid, pdu.sender);
```

## Tradeoff

`sendMessageToReturningId` makes an additional Telegram API request per
extra recipient (each HTTP round-trip costs ~500 ms). For small allow
lists (≤5 users) this is acceptable. The ReplyTargetMap ring (200 slots)
is shared among all senders — in high-volume scenarios extra recipients
consume slots faster, but the 200-slot capacity is already sized for
normal home/team use.

## Notes for handover

Changed: `src/sms_handler.cpp`,
`test/test_native/test_sms_handler.cpp` (update existing multi-user test),
`rfc/0080-extra-recipient-reply-routing.md`.
