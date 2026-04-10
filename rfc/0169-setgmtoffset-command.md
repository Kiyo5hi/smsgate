---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0169: /setgmtoffset <hours> command — runtime timezone for SMS timestamps

## Motivation

`timestampToRFC3339` in sms_codec.cpp hardcodes "+08:00" for all forwarded
SMS timestamps. Users in other timezones see incorrect local times in the
forwarded messages. `/setgmtoffset <hours>` makes the timezone configurable
at runtime without a reflash.

## Plan

Add `int gmtOffsetHours = 8` parameter to `timestampToRFC3339` (default
preserves current behaviour). Add `setGmtOffsetHours(int)` / `gmtOffsetHours_`
to `SmsHandler`. Update `sms_handler.cpp` to pass `gmtOffsetHours_` to
`timestampToRFC3339`. Add `setGmtOffsetFn(std::function<void(int)>)` to
TelegramPoller. `/setgmtoffset <hours>` validates -12 to +14 and calls
`gmtOffsetFn_(hours)`. Wire in main.cpp. Range: -12 to +14 (covers all IANA
standard offsets).
