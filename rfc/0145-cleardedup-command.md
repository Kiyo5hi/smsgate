---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0145: /cleardedup command — clear SMS dedup ring buffer

## Motivation

Complement to RFC-0144. Clears the dedup ring so the same SMS can be
received again immediately — without waiting for the window to expire
or disabling dedup entirely.

## Plan

- Add `void clearDedupBuffer()` to `SmsHandler` (zeroes dedupHashes_ and
  sets dedupHead_ = 0, dedupFilled_ = false).
- Add `setClearDedupFn(std::function<void()>)` setter to TelegramPoller.
- Command: `/cleardedup` — calls fn and replies "✅ Dedup buffer cleared."
