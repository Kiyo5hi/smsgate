---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0094: /sendall command — broadcast SMS to all aliases

## Motivation

When the operator needs to send the same SMS to everyone in their contact
book (all defined aliases), `/sendall <message>` is more convenient than
issuing individual `/send @name msg` commands.

## Design

`/sendall <body>` enqueues one SMS per alias in `SmsAliasStore`. If the store
is empty or not configured, reports error. Replies with confirmation:
"✅ Queued to N recipient(s): <preview>".

Each entry uses the standard `smsSender_.enqueue()` path with per-entry
failure/success callbacks (same as `/send`). Failures are reported
individually via chat reply.

Alias count = 0 is reported as "No aliases defined — use /addalias first."

## File changes

**`src/telegram_poller.cpp`** — add /sendall handler, add to /help  
**`src/telegram.cpp`** — register command  
**`test/test_native/test_telegram_poller.cpp`** — test /sendall
