---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0170: /loginfo command — debug log ring buffer status

## Motivation

`/logstats` gives aggregate counts but doesn't show how full the ring
buffer is or when the oldest/newest entry was received. `/loginfo` adds
a compact status line: `N/20 entries | oldest: <timestamp> | newest: <timestamp>`.
Uses `debugLog_->count()`, `kMaxEntries`, and the entry iterator.

## Plan

Add a `newestEntry()` / `oldestEntry()` accessor to `SmsDebugLog` that
returns a const pointer (nullptr if empty). Add `/loginfo` handler in
`TelegramPoller::processUpdate` that calls `debugLog_->count()` etc.
No new fn injection needed — `debugLog_` is already a member.
