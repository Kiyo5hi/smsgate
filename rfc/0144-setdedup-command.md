---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0144: /setdedup command — change SMS dedup window at runtime

## Motivation

`SmsHandler::kDedupWindowMs = 30s` prevents the same SMS arriving twice
within 30 seconds from being forwarded. During testing (sending repeated
SMS), this blocks forwarding. A runtime command avoids reflashing.

## Plan

- Change `kDedupWindowMs` in `SmsHandler` from `static constexpr` to a
  runtime field `dedupWindowMs_` defaulting to the constant.
- Add `void setDedupWindowMs(unsigned long ms)` setter.
- Add `setDedupWindowFn(std::function<void(uint32_t)>)` setter to TelegramPoller.
- Command: `/setdedup <seconds>` (0 = disable dedup, max 3600).
- In `main.cpp` wire: lambda calls `smsHandler.setDedupWindowMs(secs * 1000UL)`.
