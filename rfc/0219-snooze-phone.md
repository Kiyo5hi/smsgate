---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0219: `/snooze <phone> <min>` — temporarily suppress SMS from a number

## Motivation

Marketing SMS from a known number can arrive repeatedly during a
window (e.g. 10 messages in an hour from a bank OTP service). The
operator doesn't want to permanently block the number but wants to
suppress Telegram notifications for 30 minutes.

## Design

New commands:
- `/snooze <phone> <min>` — suppress forwarding from that phone for
  N minutes (1–480). Stored in-memory as a map: phone → expiry millis.
  Up to 20 simultaneous snoozes.
- `/unsnooze <phone>` — remove a snooze entry.
- `/snoozelist` — show all active snoozes with remaining time.

The phone number is normalized via `sms_codec::normalizePhoneNumber`
before storage. Matching is done against the normalized sender phone
in `SmsHandler::handleSmsIndex` — the handler needs a check callback
injected from main.cpp.

Implementation: `setSnoozedFn(std::function<bool(const String&)> fn)` on
`SmsHandler`. When set, the handler calls `fn(senderPhone)` before
forwarding. If it returns true, the SMS is deleted from SIM silently
(no Telegram forward, no error).

The snooze map lives in TelegramPoller (it manages the commands and
the clock). Expose `isSnoozed(phone)` as a public method for the fn.

## Notes for handover

- Snooze map: `std::map<String, uint32_t>` phone → expireMs. Cap at 20
  entries; on overflow, reject with error.
- Expired entries are checked lazily on command invocation and on each
  `tick()` (reap expired entries to keep map small).
- No NVS persistence — volatile, resets on reboot.
- `isSnoozed(phone)` normalizes phone before lookup, checks `nowMs`.
- 2 native tests: (a) snoozed entry returns true within window; (b)
  expired entry returns false after window.
