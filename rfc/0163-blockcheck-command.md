---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0163: /blockcheck <phone> command — check if a number would be blocked

## Motivation

Operators sometimes set up block rules and then wonder why SMS from a
specific number is or isn't getting through. `/blockcheck +8613...` gives
a definitive yes/no and tells which list matched, without having to wait
for an actual SMS.

## Plan

Add `setBlockCheckFn(std::function<String(const String &)>)` to TelegramPoller.
`/blockcheck <phone>` calls `blockCheckFn_(phone)` and sends the result.

In main.cpp the lambda checks both compile-time and runtime block lists using
the existing `isBlocked()` file-static, also checking `blockingEnabled_` state.
Returns a formatted string: blocked/not-blocked + which list matched.
