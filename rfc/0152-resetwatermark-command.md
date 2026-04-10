---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0152: /resetwatermark command — reset Telegram update_id watermark

## Motivation

If some Telegram updates (commands, replies) were missed due to a network
glitch, `/resetwatermark` causes TelegramPoller to re-request all updates
from the beginning of Telegram's retention window (last ~100 updates).
Useful for debugging "why didn't the bot respond to my command?"

## Plan

Add `resetWatermark()` method to TelegramPoller that sets
`lastUpdateId_ = 0` and persists the new value. The next tick will
fetch all recent updates (up to Telegram's 100-update buffer).

Command: `/resetwatermark` — calls this method directly (no fn indirection
needed since TelegramPoller owns the state). Replies
"✅ Watermark reset. Recent updates will be re-processed on next poll."
