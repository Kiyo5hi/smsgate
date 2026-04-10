---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0174: /smshandlerinfo command — SmsHandler configuration snapshot

## Motivation

/settings shows some SmsHandler parameters but not all. /smshandlerinfo gives
a focused view of all SMS handler configuration: forwarding enabled, blocking
enabled, max fail count, concat TTL, dedup window, GMT offset, fwd tag, and
lifetime SMS counts (forwarded, blocked, deduplicated).

## Plan

Add `setSmsHandlerInfoFn(std::function<String()>)` to TelegramPoller. Add
`/smshandlerinfo` handler. Wire in main.cpp with a lambda that reads all
relevant SmsHandler getters plus SmsSender.maxParts().
