---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0089: /clearqueue command

## Motivation

`/flushqueue` resets retry timers (forces immediate retry). `/cancel N`
removes one entry. But when the queue is full of permanently stuck entries
(e.g. wrong number sent many times) there is no way to wipe all entries at
once. `/clearqueue` fills that gap.

## Design

- Add `SmsSender::clearQueue() -> int` — marks all occupied slots as empty,
  fires no callbacks (intentional discard, not failure). Returns count cleared.
- Add `/clearqueue` bot command in `TelegramPoller`. Reports how many entries
  were removed.
- Distinct semantics from `/flushqueue` (which keeps entries and triggers
  immediate retry) and `/cancel N` (removes one by index).

## File changes

**`src/sms_sender.h`** — add `clearQueue()` declaration  
**`src/sms_sender.cpp`** — implement `clearQueue()`  
**`src/telegram_poller.cpp`** — add `/clearqueue` handler, add to /help  
**`src/telegram.cpp`** — register command  
**`test/test_native/test_sms_sender.cpp`** — test clearQueue  
**`test/test_native/test_telegram_poller.cpp`** — test /clearqueue command
