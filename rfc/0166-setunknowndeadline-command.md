---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0166: /setunknowndeadline <ms> command — runtime RING-without-CLIP deadline

## Motivation

`kUnknownNumberDeadlineMs = 1500ms` in CallHandler is hardcoded. Some modem
firmware delivers +CLIP more than 1.5s after RING, causing the call to be
committed as "Unknown" when the number was actually available. Others want a
shorter deadline for faster response. `/setunknowndeadline <ms>` makes this
runtime-configurable.

## Plan

Add `setUnknownDeadlineMs(uint32_t ms)` / `unknownDeadlineMs_` to
`CallHandler`. Replace the `kUnknownNumberDeadlineMs` reference in `tick()`
with the runtime field. Add `setCallUnknownDeadlineFn(std::function<void(uint32_t)>)`
to TelegramPoller. `/setunknowndeadline <ms>` validates 500–10000 and calls
`callUnknownDeadlineFn_(ms)`. Wire in main.cpp.
