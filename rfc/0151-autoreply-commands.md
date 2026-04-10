---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0151: /setautoreply, /getautoreply, /clearautoreply commands

## Motivation

Operator interface for RFC-0150 auto-reply. Commands to set/view/clear
the SMS auto-reply text from Telegram without reflashing.

## Plan

- Add `setAutoReplyGetFn`, `setAutoReplySetFn` setters to TelegramPoller.
- `/getautoreply` → calls getter, replies with current text or "(not set)".
- `/setautoreply <text>` → validates length (max 160 chars), calls setter.
- `/clearautoreply` → calls setter with empty string.
- Setter lambda in main.cpp saves to NVS "autoreply" key.
