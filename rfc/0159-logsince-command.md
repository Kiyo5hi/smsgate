---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0159: /logsince <hours> command — show log entries from the past N hours

## Motivation

`/logs N` shows the last N entries by count. `/logsince 2` shows everything
from the past 2 hours — a natural query for "what happened recently".
Requires `unixTimestamp > 0` on entries; entries with `unixTimestamp == 0`
(e.g., from startup rehydration without NTP) are omitted.

## Plan

Add `dumpBriefSince(uint32_t sinceUnix) const` to `SmsDebugLog` that walks
all entries and includes only those with `unixTimestamp >= sinceUnix`.
TelegramPoller handles `/logsince <hours>` (validates 1–168, converts to
`sinceUnix = now - hours*3600`) calling `debugLog_->dumpBriefSince(cutoff)`.
Uses `clockFn_` for current time.
