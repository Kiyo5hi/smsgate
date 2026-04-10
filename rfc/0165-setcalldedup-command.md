---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0165: /setcalldedup <seconds> command — runtime call dedup window

## Motivation

`kDedupeWindowMs = 6000ms` in CallHandler is hardcoded. Some users experience
calls where the modem sends RINGs with >6s gaps between packets, causing
multiple notifications for a single call. Others want a shorter window for
back-to-back calls. `/setcalldedup <seconds>` makes this configurable.

## Plan

Add `setDedupeWindowMs(uint32_t ms)` to `CallHandler`. Add
`setCallDedupFn(std::function<void(uint32_t)>)` to TelegramPoller.
`/setcalldedup <seconds>` validates 1–60 and calls `callDedupFn_(secs*1000)`.
Wire in main.cpp.
