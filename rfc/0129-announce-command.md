---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0129 — /announce command

## Motivation

In multi-user setups (multiple TELEGRAM_CHAT_IDS), the primary operator
wants to send a message to all authorized users simultaneously. This is
useful for broadcast announcements like "maintenance window" or "SIM
changed".

## Plan

1. Add `setAnnounceFn(std::function<bool(const String &msg)> fn)` setter
   to `TelegramPoller`. When set, `/announce <msg>` calls this fn with the
   message text. The fn sends the message to all authorized chat IDs via
   the bot. When not set, replies "(announce not configured)".

2. In `main.cpp`, wire a lambda that iterates `allowedIds[]` and calls
   `realBot.sendMessageTo(id, msg)`.

3. Tests:
   - `/announce <msg>` with fn set → calls fn and replies "✅ Announced to N users.".
   - `/announce` with no arg → usage error.
   - `/announce` without fn → "(announce not configured)".
