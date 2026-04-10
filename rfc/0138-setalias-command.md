---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0138: /setalias command — add/update an alias at runtime

## Motivation

The alias store (`SmsAliasStore`) can hold up to 10 name→phone mappings and
already has `set()` and `remove()` methods, but there is no way to add or
update aliases from Telegram without reflashing the firmware.

## Plan

Add `/setalias <name> <phone>` command to `TelegramPoller`:
- Parse name (first token after command) and phone (second token).
- Call `aliasStore_->set(name, phone)`.
- On success reply "✅ Alias @<name> → <phone> saved."
- On failure (invalid name, too long, store full) reply an error.
- Usage error when zero or one arg given.

## Notes for handover

`SmsAliasStore::set()` validates the name format itself (`isValidName`).
Phone is stored as-is (no normalisation in the store); the call site
normalises before storage so stored values match what `lookupByPhone` sees.
