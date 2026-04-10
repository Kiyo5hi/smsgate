---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0176: Show alias name in forwarded SMS headers

## Motivation

`SmsAliasStore` (RFC-0088) already maps alias names to phone numbers for
outbound `/send @alice` expansion. When an SMS arrives from a known number,
the forwarded header should show the alias name alongside the raw number:
`"alice (+447911123456) | 2024-01-15T10:30:45+08:00"` instead of just the
raw number.

## Current state

`formatBotMessage` always shows the raw `humanReadablePhoneNumber(sender)`.
`SmsHandler` has no hook for name resolution. `SmsAliasStore` has no
reverse (phone → name) lookup.

## Plan

1. **`SmsAliasStore::lookupByPhone(phone)`** — O(N) reverse scan; returns
   alias name or "" on miss. Added to the existing header-only class.

2. **`SmsHandler::setAliasFn(fn)`** — inject `String(const String&)` lookup
   callback. Called in `formatBotMessage` with the sender phone; non-empty
   result prepended as `"Name (phone)"`.

3. **`main.cpp`** — wire:
   ```cpp
   smsHandler.setAliasFn([](const String &phone) {
       return smsAliasStore.lookupByPhone(phone);
   });
   ```

4. **Tests** — `test_fwdAlias_prepended_to_forwarded_message` in
   `test_sms_handler.cpp`: set `aliasFn_` returning "alice", verify header.

## Notes for handover

- The existing `/addalias` / `/rmalias` commands already handle the alias
  CRUD; no new Telegram commands needed.
- The NVS key `"aliases"` and blob format are unchanged.
- Phone comparison is exact (case-sensitive) — same format the modem emits.
