---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0054: Reply routing from delivery confirmation messages

## Motivation

RFC-0030 stores the "✅ Queued to +X" message_id so the user can reply to it
to send another SMS to the same number. But once the SMS is delivered, the
"📨 Sent to +X" confirmation appears — and replying to THAT message returned
"Reply target expired". Adding routing from the delivery confirmation closes
the conversation loop without requiring the user to scroll back to the Queued
confirmation.

## Plan

**`src/ibot_client.h`** — add new interface method:
```cpp
virtual int32_t sendMessageToReturningId(int64_t chatId, const String &text) = 0;
```
Sends to a specific chatId and returns the new message_id (> 0 on success,
0 on failure). Combines `sendMessageTo` routing with `sendMessageReturningId`
id capture.

**`src/telegram.{h,cpp}`** — implement `sendMessageToReturningId` as
`doSendMessage(text, chatId)` (reuses the existing helper).

**`test/support/fake_bot_client.h`** — implement the fake: records
`{chatId, text}` and returns `++lastFakeMsgId_`.

**`src/telegram_poller.cpp`** — change both delivery-confirmation callbacks
(reply path and /send path) from `sendMessageTo` to
`sendMessageToReturningId`, then store `replyTargets_.put(delivId, phone)`.

## Notes for handover

`ibot_client.h`, `telegram.{h,cpp}`, `fake_bot_client.h`,
`telegram_poller.cpp` changed. Existing RFC-0032 tests pass unchanged because
`sendMessageToReturningId` records the actual chatId (not the zero sentinel
of `sendMessageReturningId`).
