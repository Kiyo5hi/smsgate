---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0100: Auto-reply SMS when call is auto-rejected

## Motivation

When the bridge auto-rejects an incoming call, the caller gets silence
or busy tone and no explanation. An optional auto-reply SMS lets the
operator configure a canned response ("I can't take calls, please text
me.") that is sent automatically via the outbound queue.

## Design

- Add `void setOnCallFn(std::function<void(const String&)> fn)` to
  `CallHandler`. When a call event is committed, call `fn(number)`.
  The number is the caller's phone or "" for unknown.
- Production (main.cpp): when `CALL_AUTO_REPLY_TEXT` is defined at
  compile time, wire the callback to enqueue an SMS to the caller.
  Skip if number is empty/unknown (can't SMS an unknown number).
- The callback fires from inside `commitRinging()` (same call path as
  the Telegram notification).

## File changes

**`src/call_handler.h`** — add `setOnCallFn()` setter and `onCallFn_` member  
**`src/call_handler.cpp`** — call `onCallFn_` in `commitRinging()`  
**`src/main.cpp`** — wire `setOnCallFn` when `CALL_AUTO_REPLY_TEXT` is defined  
**`test/test_native/test_call_handler.cpp`** — test callback fires
