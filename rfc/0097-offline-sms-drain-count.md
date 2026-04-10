---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0097: Offline SMS drain count notification

## Motivation

When the bridge reboots after being offline, `sweepExistingSms()` forwards
any SMS that arrived during the outage. The operator has no way to know how
many were forwarded without checking `/last`. A follow-up Telegram message
("📨 Drained N offline SMS.") makes this visible immediately.

## Design

- Change `SmsHandler::sweepExistingSms()` to return `int` (count of indices
  passed to `handleSmsIndex`).
- In `main.cpp` setup(), after `sweepExistingSms()`, if the count > 0 send
  a brief message: "📨 Drained N offline SMS." via `realBot.sendMessage()`.

## File changes

**`src/sms_handler.h`** — change `sweepExistingSms()` return type to `int`  
**`src/sms_handler.cpp`** — track and return count  
**`src/main.cpp`** — send follow-up message if count > 0  
**`test/test_native/test_sms_handler.cpp`** — update existing call site (void → int)
