---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0146: /forwardsim command — force-forward a SIM slot to Telegram

## Motivation

After a transient Telegram failure, an SMS may be stuck in a SIM slot.
`/simread <idx>` shows the content but doesn't forward it. This command
triggers the normal `SmsHandler::handleSmsIndex` pipeline (decode → send
to Telegram → delete on success) for a specific index.

## Plan

Add `setSmsForwardFn(std::function<bool(int)>)` setter to `TelegramPoller`.
The fn calls `smsHandler.handleSmsIndex(idx)` and returns true on success.
Command: `/forwardsim <idx>` — calls fn, replies "✅ Forwarded slot <idx>."
on success or "❌ Failed to forward slot <idx>." on failure.
Validate idx range 1–255 before calling the fn.
