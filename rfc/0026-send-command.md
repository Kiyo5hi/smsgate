---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0026: `/send` one-shot outbound SMS command

## Motivation

Every outbound SMS today requires a prior incoming SMS to reply to. `/send`
lets the user initiate a conversation from Telegram without needing a prior
message from the recipient.

## Plan

Add a `/send <number> <body>` command dispatched in
`TelegramPoller::processUpdate()` after the `/unblock` block. The argument
is extracted from `u.text` (not `lower`) to preserve Unicode characters in
the body. Calls `smsSender_.enqueue()` directly — same retry/failure path
as the reply route.

## Notes for handover

Files changed: `src/telegram_poller.cpp`, `src/telegram.cpp`,
`test/test_native/test_telegram_poller.cpp`.
