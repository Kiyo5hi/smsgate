---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0030: `/send` confirmation stored in reply-target map

## Motivation

After `/send +861380XXXX Hello`, the user receives a "✅ Queued to …" confirmation.
To send a follow-up to the same number the user would have to type `/send …` again.
If we store the confirmation message_id in the reply-target map, the user can simply
reply to that confirmation — same UX as replying to a forwarded SMS.

## Plan

In `telegram_poller.cpp`, replace `sendMessageTo` for the `/send` confirmation with
`sendMessageReturningId`, and store the returned id:

```cpp
int32_t confirmId = bot_.sendMessageReturningId(
    String("\xE2\x9C\x85 Queued to ") + phone + String(": ") + preview);
if (confirmId > 0)
    replyTargets_.put(confirmId, phone);
```

`TelegramPoller` already receives `ReplyTargetMap&` in its constructor (used for
incoming-SMS forwards), so no new wiring is needed.

## Notes for handover

Only `src/telegram_poller.cpp` changed. Tests updated to assert the reply-target map
is populated after `/send`, using `bot.lastIssuedMessageId()` rather than a
hard-coded id (FakeBotClient starts at 1000, not 0).
