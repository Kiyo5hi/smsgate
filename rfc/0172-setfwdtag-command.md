---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0172: /setfwdtag <text> command — custom SMS forward prefix tag

## Motivation

Users with multiple SMS bridges on the same Telegram bot/chat cannot tell
which device forwarded a given message. `/setfwdtag [Home]` prepends "[Home] "
to every forwarded SMS, making the source device identifiable at a glance.

## Plan

Add `fwdTag_` (String, default empty) to `SmsHandler`. Modify
`formatBotMessage` to prepend `fwdTag_ + " "` when non-empty. Add
`setFwdTagFn(std::function<void(const String &)>)` to TelegramPoller.
Add `/setfwdtag <text>` (max 20 chars) and `/clearfwdtag` commands.
Wire in main.cpp. Also expose current tag in `/settings`.
