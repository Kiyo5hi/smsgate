---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0108 — Reply to call notification to send SMS

## Motivation

RFC-0100 sends a Telegram notification for each rejected call. The user
can already reply to a forwarded SMS to send a response; it makes sense to
support the same gesture for call notifications: replying to the call
message sends an SMS to the caller.

## Plan

1. In `CallHandler::commitRinging`, after `bot_.sendMessage(msg)`, also
   try to call a new method `bot_.sendMessageReturningId(msg)` OR use the
   version that returns an ID. Actually, `IBotClient::sendMessage` returns
   bool (no ID). We need the message_id of the call notification to register
   it in the `ReplyTargetMap`.

   The clean way: use `bot_.sendMessageReturningId(msg)` (which exists on
   `IBotClient`) instead of `sendMessage`. If the return value > 0, the
   `onCallFn_` can pass both the number AND the message_id to the caller.

2. Change `setOnCallFn` signature to `void(const String &number, int32_t
   messageId)` in `call_handler.h`. This is a breaking change to the
   existing callback signature.

3. In `main.cpp`, update the `#ifdef CALL_AUTO_REPLY_TEXT` block to use
   the new two-argument signature AND register the `(messageId, number)`
   pair in `replyTargets` so future Telegram replies to the call notification
   are routed as SMS.

4. Update the test for `onCallFn_fires_with_number` to pass a two-argument
   lambda.

## Notes for handover

The `CallHandler` currently holds `IBotClient&` and calls `bot_.sendMessage`.
Switching to `sendMessageReturningId` returns an int32_t — we capture it and
pass it to `onCallFn_` as the second argument. In `FakeBotClient`, the
existing `sendMessageReturningId` returns an auto-incrementing ID starting
from 1, so existing tests still work.

Note: `sendMessageReturningId` in `IBotClient` sends to the admin chat.
`CallHandler` is fine with this — call notifications go to admin only anyway.
