---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0162: /setblockmode command — toggle SMS block list enforcement

## Motivation

When diagnosing why a specific number is not getting through, the operator
needs to temporarily disable the block list without losing all configured
entries. `/setblockmode off` suspends enforcement; `/setblockmode on`
re-enables it. The block list entries remain in place — only the
enforcement is toggled.

## Plan

Add `blockingEnabled_` bool (default `true`) to `SmsHandler` with
`setBlockingEnabled(bool)` getter/setter. Wrap the block check at line 528
of sms_handler.cpp with `if (blockingEnabled_ && ...)`. Add
`setBlockingEnabledFn(std::function<void(bool)>)` to TelegramPoller and
handle `/setblockmode on|off`. Wire in main.cpp.
