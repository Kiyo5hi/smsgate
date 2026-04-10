---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0175: /setgmtoffsetmin <minutes> command — fractional timezone offsets

## Motivation

RFC-0169's /setgmtoffset only supports whole-hour offsets. Countries like India
(UTC+5:30 = 330 min), Nepal (UTC+5:45 = 345 min), Iran (UTC+3:30 = 210 min),
and Afghanistan (UTC+4:30 = 270 min) need fractional offsets. This RFC adds a
complementary /setgmtoffsetmin <total_minutes> command.

## Plan

Change SmsHandler's gmtOffset storage from hours (int) to minutes (int), rename
gmtOffsetHours_ → gmtOffsetMinutes_, and update all callers. Update
timestampToRFC3339 to take minutes instead of hours. Update the existing
/setgmtoffset <hours> to multiply by 60. Add /setgmtoffsetmin <total_minutes>
that validates -720 to +840 (covers all IANA offsets). Update /settings and
/smshandlerinfo to display minutes-form when non-whole-hour.
