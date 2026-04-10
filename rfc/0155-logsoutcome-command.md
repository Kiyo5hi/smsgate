---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0155: /logsoutcome <keyword> command — filter log by outcome

## Motivation

`/logstats` shows totals. `/logsoutcome fail` shows which SMS actually
failed — the raw entries filtered by outcome substring. Mirrors
`/history <phone>` (sender filter) but filters by outcome instead.

## Plan

Add `dumpBriefByOutcome(size_t n, const String &keyword) const` to
`SmsDebugLog`. TelegramPoller handles `/logsoutcome <keyword>` using
`debugLog_->dumpBriefByOutcome(10, keyword)` directly (no setter fn).
