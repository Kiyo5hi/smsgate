---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0101 — Alias name character validation

## Motivation

`SmsAliasStore::set()` currently only checks name length. Names containing
spaces (`"my contact"`), at-signs (`"@alice"`), slashes, or other
non-alphanumeric characters would:

- Break `/addalias` argument parsing (the first space delimits name from phone)
- Collide with the `@name` prefix used in `/send` and `/test`
- Produce confusing output in `/aliases` listings

Rejecting invalid names early, with a clear error, prevents silent misbehaviour.

## Plan

1. Add `static bool isValidName(const String &name)` to `SmsAliasStore`.
   Allowed characters: `[a-zA-Z0-9_-]` (alphanumeric plus underscore plus
   hyphen). Empty names are already rejected by the length check.

2. Call `isValidName()` at the top of `set()`, before the existing length
   guard, returning `false` on failure.

3. Update the `/addalias` error reply in `TelegramPoller` to mention the
   character constraint.

4. Add native tests: valid names pass, names with spaces / @ / dots / slashes
   fail, `set()` rejects invalid names.

## Notes for handover

`isValidName` is public so `TelegramPoller` can detect the failure reason and
give a targeted error instead of the generic "name/number too long" message.
