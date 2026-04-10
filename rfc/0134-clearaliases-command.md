---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0134 — /clearaliases command

## Motivation

When migrating to a new alias set or resetting for a new user, the
operator wants to remove all aliases at once without running
`/rmalias` for each entry individually.

## Plan

1. Wire `/clearaliases` in `TelegramPoller`. When `aliasStore_` is set,
   removes all aliases and replies "✅ Cleared N aliases." (or "(no
   aliases)" if already empty). When not set, replies "(alias store not
   configured)".

2. Implement `clear()` method on `SmsAliasStore` that resets `count_`
   to 0 and saves the empty blob.

3. Tests:
   - `/clearaliases` with populated store → removes all and confirms.
   - `/clearaliases` with empty store → "(no aliases)".
   - `/clearaliases` without store → "(alias store not configured)".
