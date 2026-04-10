---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0088: Phone number aliases for /send and /test

## Motivation

Typing full E.164 phone numbers for `/send +447911123456 Hello` is
error-prone and tedious in daily use. Aliases let the operator define
short names (e.g. `alice`) and use `@alice` instead of the full number.

## Design

A new `SmsAliasStore` class (header-only, plain C++):
- Up to `kMaxAliases = 10` entries
- Each entry: name (max 16 chars, lowercase) + phone (max 21 chars)
- Persisted via `IPersist` as a compact blob (key: "aliases")
- Methods: `set(name, phone)`, `remove(name)`, `lookup(name) -> String`
  (empty on miss), `list() -> String` (human-readable), `count()`

New bot commands (admin-only):
- `/addalias <name> <number>` — add or replace an alias
- `/rmalias <name>` — remove an alias
- `/aliases` — list all aliases

Alias expansion in `/send` and `/test`:
- If phone arg starts with `@`, look up the rest in the alias store.
  If found, use the stored phone. If not found, report error.

## File changes

**`src/sms_alias_store.h`** — new header-only class
**`src/telegram_poller.h`** — add `setAliasStore(SmsAliasStore*)` setter
**`src/telegram_poller.cpp`** — add /addalias, /rmalias, /aliases handlers;
  expand `@name` in /send and /test
**`src/main.cpp`** — instantiate SmsAliasStore, load from persist, wire up
**`src/telegram.cpp`** — register commands, update log string
**`test/test_native/test_telegram_poller.cpp`** — new tests (or existing)

## Notes for handover

No new compile-time config needed. Aliases are runtime-only (NVS).
