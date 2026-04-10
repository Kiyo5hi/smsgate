---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0139: /delalias command — remove a single alias at runtime

## Motivation

Complement to RFC-0138. Allows removing one alias without clearing the
entire store (which RFC-0134 `/clearaliases` does).

## Plan

Add `/delalias <name>` command to `TelegramPoller`:
- Parse the name argument.
- Call `aliasStore_->remove(name)`.
- If true (found and removed): reply "✅ Alias @<name> removed."
- If false (not found): reply "(alias @<name> not found)".
- Usage error when no arg given.

## Notes for handover

`SmsAliasStore::remove()` is case-insensitive (delegates to `nameMatch`).
