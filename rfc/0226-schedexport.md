---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0226: /schedexport and /schedimport

## Motivation

The scheduled queue is persisted in NVS but is lost on factory reset or when upgrading
firmware that changes the NVS blob format (e.g. RFC-0221 bumped to v0x02). An operator
with recurring or date-specific scheduled SMS has no way to back them up.

`/schedexport` prints each occupied slot as a `/scheduleat` or `/schedulesend` command
that can be copy-pasted back via `/schedimport` (batch import from a block of commands).

## Plan

1. `/schedexport` — for each occupied slot, emit one line:
   - If `repeatIntervalMs > 0` (repeating): `/recurring <intervalMin> <phone> <body>`
   - If `wallTimeFn_` is set and returns valid epoch: `/scheduleat YYYY-MM-DD HH:MM <phone> <body>`
   - Otherwise: `/schedulesend <remainingMin> <phone> <body>`
   - Header line: "📋 Scheduled queue export (N slots):"
   - Empty queue: "(no scheduled SMS)"

2. `/schedimport <commands>` — accept a block of commands (newline-separated) and execute
   each one that starts with `/scheduleat`, `/schedulesend`, or `/recurring`.
   - Each recognized line is processed inline (not via pollUpdates) as if the operator sent it.
   - Report: "✅ Imported N slot(s), skipped M line(s)."
   - Skip malformed lines silently (already have error handling in the individual commands).

   NOTE: `/schedimport` is complex to implement (re-dispatching commands internally).
   Defer to RFC-0227 and implement only `/schedexport` in this RFC.

3. Help entries.
4. Tests: export with occupied slots, export with empty queue, repeating slot exports as /recurring.

## Notes for handover

The `/schedexport` format is human-readable and re-entrant. Exporting then factory-resetting
and running `/schedimport <paste>` is a complete backup/restore cycle.
Absolute timestamps (from `/scheduleat`) allow restoring the exact intended fire time,
not just a relative delay.
