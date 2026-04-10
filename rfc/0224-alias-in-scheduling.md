---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0224: @alias support in scheduling commands

## Motivation

`/send @alice body` already resolves `@alice` to a phone number via the alias store.
The scheduling commands (`/schedulesend`, `/sendafter`, `/scheduleat`, `/recurring`)
do not — they call `normalizePhoneNumber()` directly and reject `@` prefixes.
Operators who use aliases should be able to write `/schedulesend 60 @alice body`.

## Plan

1. Add a file-static helper `resolvePhone(rawPhone, aliasStore*, errorOut&) -> String`
   - If rawPhone starts with `@`: look up alias; fill errorOut on miss; return "".
   - Otherwise: call `sms_codec::normalizePhoneNumber(rawPhone)`.
2. Replace `sms_codec::normalizePhoneNumber(...)` with `resolvePhone(...)` in:
   - `/schedulesend` (line ~2828)
   - `/sendafter` (line ~2961)
   - `/scheduleat` (line ~3087)
   - `/recurring` (line ~3395)
   - `/schedrename` (line ~3203) — allows renaming to an alias
3. Tests: alias resolves in /schedulesend, unknown alias in /schedulesend replies error.

## Notes for handover

The `/multicast` command already does its own comma-split + alias resolution loop
so it's not included in this RFC.
