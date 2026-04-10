---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0053: /help command

## Motivation

The bridge currently shows a command list only as an error reply when a user
sends an unrecognized message. New users have no clean way to discover the
available commands. `/help` provides an explicit, formatted command reference.

## Plan

**`src/telegram_poller.cpp`** — add handler before `/ping`:
```
/help → lists all commands with one-line descriptions
```
Block-list commands (`/blocklist`, `/block`, `/unblock`) are only shown when
`smsBlockMutator_` is set — consistent with the existing error-reply behaviour.

**`src/telegram.cpp`** — register `/help` command.

## Notes for handover

Only `src/telegram_poller.cpp` and `src/telegram.cpp` changed. No test
changes needed.
