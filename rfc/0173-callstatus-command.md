---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0173: /callstatus command — CallHandler configuration and state snapshot

## Motivation

There is no Telegram command to inspect the current call handler configuration
(callNotifyEnabled, dedupeWindowMs, unknownDeadlineMs, callsReceived). While
/settings shows some of this, /callstatus gives a focused view of all call-
related parameters including the live state (idle/ringing/cooldown).

## Plan

Add `setCallStatusFn(std::function<String()>)` to TelegramPoller. Add
`/callstatus` handler. Wire in main.cpp with a lambda that reads all
CallHandler getters and formats a multi-line status string.
