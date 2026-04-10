---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0171: /smsrate command — SMS forwarding rate from debug log

## Motivation

There is no way to gauge how busy the bridge is via Telegram without
looking at `/logstats` + `/logsince`. `/smsrate` computes forwarded SMS
per hour and per day by examining the unixTimestamps of "fwd OK" entries
in the debug log and comparing with `time(nullptr)`.

## Plan

Add `rate(uint32_t nowUnix)` method to `SmsDebugLog` that returns a
formatted rate string. Or implement it directly in TelegramPoller using
a `clockUnixFn_` (already available via `time(nullptr)` in the poller's
context). Since we need `time()` from the embedded side, add
`setUnixClockFn(std::function<uint32_t()>)` to TelegramPoller. Use it
in `/smsrate` to get current epoch, then count "fwd OK" entries in the
last 1h and 24h windows.
