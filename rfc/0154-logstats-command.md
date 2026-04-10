---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0154: /logstats command — aggregate SMS log statistics

## Motivation

`/logs` shows raw entries. `/logstats` shows a summary: how many SMS
forwarded, failed, blocked, deduplicated — broken down from the debug
log ring buffer. Complements the live counters in `/count`.

## Plan

Add `stats() const` method to `SmsDebugLog` that returns a formatted
string counting entries by outcome substring ("fwd", "fail", "blocked",
"dup"). TelegramPoller uses `debugLog_->stats()` directly (no setter fn).
