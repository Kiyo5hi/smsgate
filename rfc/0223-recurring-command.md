---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0223: /recurring <interval_min> <phone> <body>

## Motivation

Setting up a repeating scheduled SMS today requires two commands:
  /schedulesend 60 +1234 Daily ping
  /schedrepeat 1 60

`/recurring` combines both into one command for a better operator UX.

## Plan

1. Add `/recurring <interval_min> <phone> <body>` command handler.
   - Validates interval: 1–10080 minutes (1 min to 7 days).
   - Validates phone (normalizes), validates body (non-empty).
   - Finds a free slot, sets `sendAtMs = nowMs + interval_min * 60000`,
     `repeatIntervalMs = interval_min * 60000`.
   - Calls `persistSchedFn_`.
   - Reply: "🔁 Recurring SMS to <phone> every Xm (slot N/5). First send in Xm."
2. Help entry.
3. Tests: creates repeating slot, interval out of range rejected.

## Notes for handover

Essentially `/schedulesend interval phone body` + `/schedrepeat slot interval`
in one step. The slot's `sendAtMs` and `repeatIntervalMs` are both set to
`interval_min * 60000`.
