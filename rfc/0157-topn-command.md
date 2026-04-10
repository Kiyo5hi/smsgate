---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0157: /topn [N] command — top N SMS senders by frequency

## Motivation

`/logstats` shows totals by outcome. `/topn` shows which senders appear
most often in the log — useful for spotting SMS spam or understanding
usage patterns. TelegramPoller can implement this directly using
`debugLog_` without a setter fn.

## Plan

Add `topSenders(size_t n) const` to `SmsDebugLog` that counts occurrences
per sender (case-sensitive exact match) and returns the top n senders
sorted by descending count as a formatted string. TelegramPoller handles
`/topn [N]` (default 5, max 10) calling `debugLog_->topSenders(n)`.
