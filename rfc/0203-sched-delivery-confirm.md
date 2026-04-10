---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0203: Telegram notification when scheduled SMS fires

## Motivation

When a scheduled SMS fires (slot drain in `tick()`), the user gets no
feedback. They scheduled it 30+ minutes ago and don't know whether it
sent. A Telegram notification closes the gap.

## Design

In `TelegramPoller::tick()`, when `smsSender_.enqueue(slot.phone, slot.body)`
succeeds and the slot is freed, send a Telegram notification:

```
✅ Scheduled SMS to +1234 sent (body preview...).
```

Use `bot_.sendMessage(...)` (to the admin chat, not a specific chatId).
Body preview is truncated at 60 chars.

No new setter needed — `bot_` is already a member. The notify is
best-effort: if `sendMessage` fails, we don't retry (the SMS was already
enqueued successfully).

## Notes for handover

- Use `bot_.sendMessage(text)` which sends to the admin/primary chat.
  This is consistent with how call-notification messages are sent.
- Body preview: `body.length() > 60 ? body.substring(0, 60) + "..." : body`.
- The persist call (`if (schedFired && persistSchedFn_)`) already exists;
  add the notification in the same `if (smsSender_.enqueue(...))` block.
