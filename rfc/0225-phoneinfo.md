---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0225: /phoneinfo <phone|@alias>

## Motivation

When working with a phone number, operators want a single command that
consolidates everything the bridge knows about it: block list status,
snooze status, alias names, and recent log activity. Today this requires
running /blockcheck, checking /snoozelist, searching /history, and
scanning /aliases separately.

## Plan

1. Add `/phoneinfo <phone|@alias>` command handler.
   - Resolves alias via `resolvePhone()` (RFC-0224).
   - Queries:
     a. Block list: calls `blockCheckFn_` if set; shows "BLOCKED" / "allowed".
     b. Snooze: calls `isSnoozed(phone)`, shows remaining time from snoozeList_.
     c. Alias: reverse-lookup in aliasStore_ — shows all aliases that resolve to
        this number (or "(none)").
     d. Recent log: calls `debugLog_->dumpBriefFiltered(phone, 3)` if debug log
        set; shows last 3 entries (or "(no log)").
   - Reply is a compact multi-section message.
2. Help entry.
3. Tests: shows block status, shows snooze, shows alias, no log configured.

## Notes for handover

`SmsAliasStore` has `list()` which returns a formatted string. There is no
reverse-lookup method — need to scan all entries. Use `SmsAliasStore::getAll()`
or similar if available; otherwise call list() and scan for the phone in the text.
Actually, add a `reverseAlookup(phone) -> String` helper to SmsAliasStore that
returns a comma-separated list of alias names that map to the given phone.
