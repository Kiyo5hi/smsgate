---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0117 — /history <filter> command

## Motivation

`/last N` shows the N most-recent SMS in the debug log regardless of
sender. When an operator wants to see all exchanges with a specific
contact — or audit activity from a given number — they must scroll
through the full log. `/history <filter>` filters the log by phone
number substring, showing only entries whose `sender` field contains
the given string.

## Plan

1. Add `String dumpBriefFiltered(size_t n, const String &filter) const`
   to `SmsDebugLog`. Walks the ring newest-first, collects entries
   where `e.sender.indexOf(filter) >= 0`, stops after `n` matches or
   after exhausting all entries. Format: same one-line-per-entry layout
   as `dumpBrief`.

2. In `telegram_poller.cpp`, add a `/history` handler:
   - Usage: `/history <filter>` where filter is a phone number
     substring (e.g. `+8613`, `138000`, or the full number).
   - If no `debugLog_` → reply "debug log not configured".
   - If no argument → reply usage hint.
   - Otherwise call `debugLog_->dumpBriefFiltered(10, filter)` and
     reply with the result.

3. Tests:
   - `dumpBriefFiltered` returns matching entries only.
   - `dumpBriefFiltered` returns empty message when no match.
   - `/history` handler calls log and replies.
   - `/history` with no arg sends usage hint.
   - `/history` with no log set replies "not configured".

## Notes for handover

`filter` is a raw substring match on the `sender` field — case-sensitive,
no normalization. For typical E.164 numbers this is fine; the operator
passes the same format that appears in the log.
