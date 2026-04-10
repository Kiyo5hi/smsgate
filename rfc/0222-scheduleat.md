---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0222: /scheduleat <YYYY-MM-DD HH:MM> <phone> <body>

## Motivation

`/sendafter HH:MM` schedules for a specific UTC time today (wraps to tomorrow if past).
There is no way to schedule for a specific future date — e.g. a birthday SMS, a reminder
on a known date. `/scheduleat` fills this gap with a full date+time argument.

## Current state

`/sendafter` parses `HH:MM` using `wallTimeFn_` to compute seconds-until-target as a
`uint32_t sendAtMs` delta from `millis()`. The format is limited to today/tomorrow.

## Plan

1. Add `/scheduleat <YYYY-MM-DD HH:MM> <phone> <body>` command handler.
   - Requires `wallTimeFn_` (NTP synced); reply error if not.
   - Parse date string: year, month, day, hour, minute — all UTC.
   - Validate ranges: year 2020–2099, month 1–12, day 1–31, hour 0–23, min 0–59.
   - Convert to Unix timestamp via a simple proleptic Gregorian day-count helper
     (no `mktime` — not reliably available in Arduino environment for arbitrary dates).
   - Compute `sendAtMs = millis() + (targetUnix - wallNow) * 1000`.
   - Reject if target is in the past or more than 365 days ahead.
   - Accept if within 30 min of now (treat as "immediately").
   - Find a free slot, populate phone+body+sendAtMs, call `persistSchedFn_`.
   - Reply with confirmation including ETA string.
2. Help entry.
3. Tests: future date, past date rejected, invalid format rejected, no-NTP error.

## Notes for handover

Use the proleptic Gregorian formula for days since epoch (Jan 1 1970):
  days = 365*(Y-1970) + leap_years_before(Y) + day_of_year(Y,M,D) - 1
  unix = days * 86400 + H*3600 + M*60
where leap_years_before(Y) = (Y-1969)/4 - (Y-1901)/100 + (Y-1601)/400

The Arduino `time.h` `mktime` has undefined behavior for years before 1902 and
implementation-defined behavior for future years on some platforms — safer to
compute manually.
