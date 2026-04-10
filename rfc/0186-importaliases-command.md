---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0186: `/importaliases` — batch alias import

## Motivation

`/addalias` adds one alias at a time. Initial setup with many contacts
requires N round trips. `/exportaliases` already produces `name=phone`
lines; there is no symmetric import command. A user who migrates devices
or sets up a new bot token should be able to paste their export back
as a single message.

## Plan

### Command format

```
/importaliases
Alice=+13800138000
Bob=+14155551234
```

All text after `/importaliases` (same line or subsequent lines) is
split by newlines. Each line is parsed as `name=phone` (first `=`
is the separator). Invalid lines are counted and skipped silently.
The leading `/importaliases` word itself is skipped.

### Validation

Reuses existing `SmsAliasStore::isValidName(name)` and
`sms_codec::normalizePhoneNumber(phone)`. Lines where the name is
invalid or the store is full are counted as "skipped".

### Reply

```
✅ Imported 3 aliases, skipped 1 invalid line.
```

Or if all fail:

```
❌ No valid aliases found. Format: name=phone (one per line)
```

### No new fns needed

`aliasStore_` is already wired in the poller. The handler lives entirely
inside `processUpdate`.

## Notes for handover

- Comma-separated input is NOT supported — only newline-separated, to
  match the `/exportaliases` output format.
- Store capacity is still 10 aliases max (SmsAliasStore limit). Entries
  beyond capacity are counted as skipped.
