---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0091: Session outbound sent/failed counters

## Motivation

The SMS section of `/status` shows forwarded and queued counts but nothing
about outbound SMS delivery outcomes. Adding per-session `sentCount` and
`failedCount` on `SmsSender` makes it easy to spot a persistently failing
outbound path without trawling the debug log.

## Design

- Add `int sentCount_` and `int failedCount_` to `SmsSender` (not persisted —
  session-only, resets on reboot).
- Increment `sentCount_` inside `drainQueue` when `onSuccess` fires.
- Increment `failedCount_` inside `drainQueue` when `onFinalFailure` fires.
- Add accessors `int sentCount() const` and `int failedCount() const`.
- Show in `/status` SMS section: `Outbound: N sent, N failed`.

## File changes

**`src/sms_sender.h`** — add counters and accessors  
**`src/sms_sender.cpp`** — increment in drainQueue  
**`src/main.cpp`** — add outbound line in statusFn SMS section  
**`test/test_native/test_sms_sender.cpp`** — tests for counter increments
