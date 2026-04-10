---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0255: WDT kick per extra-recipient send in forwardSingle / forwardConcat

## Motivation

`SmsHandler::forwardSingle()` and `forwardConcat()` forward each
incoming SMS to the admin recipient via `sendMessageReturningId()`, then
fan out to up to `extraRecipientCount_` extra recipients (RFC-0070) via
`sendMessageToReturningId()`.

Each `sendMessageToReturningId()` call can block up to ~23 s (15 s TCP
connect + 4 s header + 4 s body drain).  With `TELEGRAM_CHAT_IDS`
defining up to 10 allowed users, `extraRecipientCount_` can be up to 9.

9 × 23 s = **207 s** from the WDT kick before `handleSmsIndex()` to
completion, which would trip the 120 s watchdog every time Telegram is
slow and more than ~4 extra recipients are configured.

`noteTelegramFailure()` also calls `bot_.sendMessage()` for the
pre-reboot "N/M consecutive failures" warning, which adds another ~23 s.
If called right after a multi-recipient forward attempt, the gap from
the last WDT kick could exceed 120 s.

## Plan

Add `#ifdef ESP_PLATFORM esp_task_wdt_reset(); #endif` before:

1. Each `sendMessageToReturningId()` call in the fan-out loop of
   `forwardSingle()`.
2. The same fan-out loop in `forwardConcat()`.
3. The `bot_.sendMessage()` call inside `noteTelegramFailure()` for the
   pre-reboot warning.

Uses the established `#ifdef ESP_PLATFORM` guard pattern (same as
RFC-0248 in `sweepExistingSms()`).

## Notes for handover

The existing WDT kick before `handleSmsIndex()` at every call site
covers the initial `sendMessageReturningId()` call (admin recipient).
The new per-extra-recipient kicks cover the subsequent fan-out loop.
Combined, the worst-case gap is now a single `sendMessage*()` duration
(~23 s), regardless of recipient count.
