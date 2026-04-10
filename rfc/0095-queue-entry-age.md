---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0095: Queue entry age in /queue output

## Motivation

The `/queue` command shows pending entries but not how long they have been
waiting. Adding the queue age (e.g. "queued 47s ago") helps the operator
diagnose stuck entries and decide whether to `/cancel` them.

## Design

- Add `uint32_t queuedAtMs` to `SmsSender::OutboundEntry`; set to `nowMs`
  when `enqueue()` is called. (Note: `enqueue()` doesn't take a clock param
  today — use `(uint32_t)millis()` in production or inject via a new clock
  arg for testability.)
- Actually: simpler — pass `nowMs` at drain time; for entry age track it at
  enqueue. But `enqueue()` doesn't take a time parameter. The cleanest approach:
  store `queuedAtMs` in the entry set to 0 (unset) and fill it in `drainQueue()`
  on the entry's first drain attempt (when `e.attempts == 0`). Not quite right
  either.
- Simplest: call `(uint32_t)millis()` directly in `enqueue()` since SmsSender
  already lives in a firmware context. For testability, add an overload or use
  the inject pattern. Actually: set `queuedAtMs = 0` at enqueue, compute age
  as `nowMs - queuedAtMs` at drain time, but we need the timestamp at enqueue.
- Resolution: expose a `SmsSender::ClockFn` (same pattern as TelegramPoller)
  or just store `queuedAtMs` directly by calling a private clock — but that
  breaks host tests.
- **Chosen approach**: add `uint32_t queuedAtMs` set to 0. In `drainQueue()`,
  on first visit (`e.attempts == 0`), set `e.queuedAtMs = nowMs`. Then `/queue`
  shows `nowMs - e.queuedAtMs` seconds as the age. This is slightly inaccurate
  (counts from first drain, not enqueue) but is negligible (~3s at most).
- Add `uint32_t queuedAtMs` to `QueueSnapshot` and calculate
  `nowMs - queuedAtMs` in the `/queue` handler.

## File changes

**`src/sms_sender.h`** — add queuedAtMs to OutboundEntry and QueueSnapshot  
**`src/sms_sender.cpp`** — set queuedAtMs on first drain; include in snapshot  
**`src/telegram_poller.cpp`** — show age in /queue output  
**`test/test_native/test_sms_sender.cpp`** — test queuedAtMs is set
