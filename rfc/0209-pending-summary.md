---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0209: `/pending` — terse summary of all pending work items

## Motivation

Getting a quick snapshot of "what's in flight" requires running `/queue`,
`/schedqueue`, and `/concat` separately. `/pending` collapses these into
a single line: `Queue: 2/8 | Sched: 1/5 | Concat: 1`.

## Design

New command: `/pending`

Output format (all on one line):
```
📋 Queue: 2/8 | Sched: 1/5 | Concat: 1
```
- `Queue: N/8` — outbound retry queue depth (SmsSender)
- `Sched: N/5` — occupied scheduled SMS slots (TelegramPoller)
- `Concat: N` — in-flight concat reassembly groups (SmsHandler)

All three components are always shown. If nothing is pending:
```
📋 All clear (nothing pending)
```

Implemented via new `setPendingFn(std::function<String()> fn)` on
TelegramPoller, wired in main.cpp. The fn returns the formatted String.

## Notes for handover

- Uses existing `smsSender.queueSize()`, `telegramPoller->getSchedQueue()`,
  and `smsHandler.concatKeyCount()`.
- No new subsystem APIs needed.
