---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0125 — /me command

## Motivation

When setting up a new device or debugging authorization issues, the
operator needs to know their own Telegram fromId and chatId to paste
into `secrets.h`. `/me` replies with those IDs directly without
needing a third-party bot.

## Plan

1. Add `/me` handler to `TelegramPoller`. No setter needed — the IDs
   come directly from the update (u.fromId, u.chatId).

2. Reply format:
   ```
   👤 fromId: 123456789 | chatId: 123456789
   ```

3. This command is intentionally allowed even for unauthorized users —
   it's read-only, returns only the caller's own IDs, and is the only
   way to self-onboard. The watermark still advances.

4. Tests:
   - `/me` replies with fromId and chatId.
   - `/me` works even for unauthorized users (but still advances watermark).
