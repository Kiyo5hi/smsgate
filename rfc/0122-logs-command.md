---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0122 — /logs command

## Motivation

`/history <filter>` requires a sender filter — it can't show a general
recent-activity view. `/logs` shows the last N entries from the SMS
debug log with no filtering, giving the operator a quick snapshot of
recent SMS activity.

## Plan

1. Wire `/logs [N]` in `TelegramPoller`. When `debugLog_` is set and
   non-empty, calls `debugLog_->dumpBrief(N)` where N defaults to 10
   and is capped at 50. When `debugLog_` is null, replies
   "(debug log not configured)".

2. No new setter needed — reuses the existing `setDebugLog` path.

3. Tests:
   - `/logs` with log set → replies with dump.
   - `/logs 3` parses N correctly.
   - `/logs` without log → replies "(debug log not configured)".
