---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0178: /logdate command — filter debug log by calendar date

## Motivation

`/logsince <hours>` filters by relative time. Operators often want to look
up all SMS from a specific date (e.g., "what happened on April 8th?") without
calculating how many hours ago that was. `/logdate YYYY-MM-DD` fills this gap.

## Current state

`SmsDebugLog::dumpBriefSince(sinceUnix)` shows all entries on or after a
timestamp. No range-bounded variant exists. No `/logdate` command exists.

## Plan

1. **`SmsDebugLog::dumpBriefRange(since, until)`** — like `dumpBriefSince` but
   restricted to entries in `[since, until)`. Entries outside the window or
   with `unixTimestamp == 0` are skipped.

2. **`TelegramPoller`** — add `/logdate YYYY-MM-DD` command:
   - Parse date string into UTC midnight Unix timestamp via `parseDateUnix`.
   - Call `debugLog_->dumpBriefRange(dayStart, dayStart + 86400)`.
   - `parseDateUnix(YYYY, MM, DD)` — inline helper, uses proleptic Gregorian
     formula (same algorithm as the epoch formatter in `dumpBriefSince`).

3. **Tests** — `test_dumpBriefRange_*` in test_sms_debug_log.cpp;
   `test_TelegramPoller_logdate_*` in test_telegram_poller.cpp.

## Notes for handover

- UTC only — the log stores UTC Unix timestamps. Timezone-aware date filtering
  would require the GMT offset and adds complexity; UTC is sufficient.
- The command warns the user: "(note: timestamps in UTC)" at the end.
- Invalid date formats reply with a usage error.
