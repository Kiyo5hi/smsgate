---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0156: /simstatus command — SIM card and network status snapshot

## Motivation

`/health` shows WiFi/heap/uptime. There is no command to see the cellular
side: ICCID, IMSI, operator name, CSQ, and registration state. This
information is useful when diagnosing why SMS are not being received.

## Plan

Add `setSimStatusFn(std::function<String()>)` to TelegramPoller.
`/simstatus` calls `simStatusFn_()` and sends the result.
In main.cpp the lambda queries:
- `AT+CCID` → ICCID
- `AT+CIMI` → IMSI
- `AT+COPS?` → operator name
- `AT+CSQ` → signal quality
Returns a formatted multi-line string.
