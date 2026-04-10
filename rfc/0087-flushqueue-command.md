---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0087: /flushqueue command — reset retry timers

## Motivation

When the modem loses connectivity, outbound SMS queue up with
exponential backoff. After a brief outage the entries may be scheduled
for retry in 8 or 16 seconds. `/flushqueue` resets all retry timers to
zero, so the next `drainQueue` call (within ms) attempts all of them
immediately. Saves up to 16 seconds of unnecessary wait.

## Plan

**`src/sms_sender.h`**:
- Add `void resetRetryTimers();` — sets `nextRetryMs = 0` for all
  occupied entries.

**`src/sms_sender.cpp`**:
- Implement `SmsSender::resetRetryTimers()`.

**`src/telegram_poller.cpp`**:
- Add `/flushqueue` handler: call `smsSender_.resetRetryTimers()` and
  send confirmation "🔄 Retry timers reset. Queue will drain on next tick."
- Add `/flushqueue — Immediately retry all pending outbound SMS` to /help.

**`src/telegram.cpp`**:
- Register `/flushqueue` command.
- Update Serial log string.

## Notes for handover

Changed: `src/sms_sender.{h,cpp}`, `src/telegram_poller.cpp`,
`src/telegram.cpp`, `rfc/0087-flushqueue-command.md`.
