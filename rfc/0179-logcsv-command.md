---
status: in-progress
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0179: /logcsv command — export debug log as CSV

## Motivation

`/debug` shows the full log in human-readable format. Operators sometimes
want to export the data for analysis in a spreadsheet or script.
`/logcsv` exports all entries as comma-separated values (unix_ts,sender,outcome,chars).

## Current state

No CSV export exists. `SmsDebugLog` has `dump()` (verbose) and `dumpBrief()`
(compact human-readable), but no machine-parseable format.

## Plan

1. **`SmsDebugLog::dumpCsv()`** — returns all entries oldest-first as:
   ```
   unix_ts,sender,outcome,chars
   1775606400,+8613800138000,fwd OK,42
   ```
   Header row first. Entries with `unixTimestamp == 0` use `0` for the ts.
   Commas in outcome/sender fields escaped by quoting the field if needed.

2. **`TelegramPoller`** — add `/logcsv` command that calls `debugLog_->dumpCsv()`.

3. **Tests** — `test_dumpCsv_*` in test_sms_debug_log.cpp;
   `test_TelegramPoller_logcsv_*` in test_telegram_poller.cpp.

## Notes for handover

- CSV output for 20 entries stays under 4096 chars (Telegram message limit).
- No quoting needed in practice: phone numbers and outcomes don't contain commas.
  Keep it simple: no quoting, just raw CSV.
