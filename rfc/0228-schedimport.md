---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0228: /schedimport

## Motivation

RFC-0226 implemented `/schedexport` but deferred `/schedimport`.
An operator can now export the scheduled queue but cannot restore it after
a factory reset or firmware upgrade without re-typing each command manually.
`/schedimport` closes the backup/restore loop.

## Plan

1. `/schedimport <commands>` — accept a block of newline-separated commands
   from the message body and execute each one that starts with `/scheduleat`,
   `/schedulesend`, or `/recurring`.
   - Each recognized line is dispatched as if the operator sent it individually
     via a fake `TelegramUpdate` with the same `chatId` and `fromId`.
   - Report: "✅ Imported N slot(s), skipped M line(s)."
   - Skip blank lines and unrecognized lines silently.

2. Implementation approach:
   - Extract the body after `/schedimport`.
   - Split on `\n`.
   - For each line: trim, check prefix, call `processLine(chatId, fromId, line)`.
   - `processLine` constructs a synthetic `TelegramUpdate` and calls
     `processUpdate()` recursively — but we must NOT recurse for `/schedimport`
     itself (guard against infinite recursion).
   - Count how many lines were imported vs skipped.

3. Help entry.

4. Tests: import block with mixed lines, empty body, unknown lines skipped.

## Notes for handover

The recursive `processUpdate` approach reuses all existing validation in
`/schedulesend`, `/scheduleat`, and `/recurring`, so phone normalization,
alias resolution, and queue-full handling all work for free.
Guard: if the synthetic update text starts with `/schedimport`, skip it
(prevents accidental infinite recursion).
