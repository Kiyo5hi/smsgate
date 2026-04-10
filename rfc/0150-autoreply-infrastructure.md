---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0150: SMS auto-reply infrastructure

## Motivation

When the operator is unavailable, an SMS auto-acknowledgment
("Message received, will reply from Telegram") avoids senders wondering
if their message got through. This RFC adds the infrastructure:
a `setOnSenderFn` callback in SmsHandler (fires with sender phone after
each successful forward) and the `s_autoReplyText` static in main.cpp.

## Plan

- Add `setOnSenderFn(std::function<void(const String &phone)>)` to
  `SmsHandler`. Called after each successful single-part or concat forward.
  Does NOT fire for blocked/deduplicated messages.
- In `main.cpp` wire it: if `s_autoReplyText.length() > 0`, enqueue the
  auto-reply via `smsSender.enqueue(phone, s_autoReplyText)`.
- `s_autoReplyText` is loaded from NVS key "autoreply" at startup.
