---
status: proposed
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0271: Show GSM-7 / UCS-2 character limits in reply-required help message

## Motivation

When a user sends a non-reply, non-command Telegram message to the bot, they
receive a generic "Reply to a forwarded SMS to send a response" hint.  The hint
doesn't mention the character limits (160 chars for GSM-7, 70 for UCS-2), so
users may type a long message and be surprised when it's split into multiple
parts.

## Plan

Extend the help string that `TelegramPoller::processUpdate()` sends when it
falls through to the "no matching command" path (the reminder message):

```
Reply to a forwarded SMS to send a response.
Limits: 160 chars (GSM-7) or 70 chars (Unicode) per part; up to 10 parts.
Type /help for available commands.
```

One-liner change; no new state, no modem interaction.

## Notes for handover

- Low-value cosmetic improvement; deprioritise behind stability fixes.
- The relevant code is near the end of `processUpdate()` in
  `telegram_poller.cpp`.
