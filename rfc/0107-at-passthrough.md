---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0107 — /at command (admin AT passthrough)

## Motivation

When diagnosing carrier or modem issues (wrong APN, CLIP disabled, USSD
format, etc.) it is necessary to send raw AT commands and observe the
response. Currently this requires physical access to a serial monitor.
An `/at <cmd>` command lets the admin diagnose and test the modem remotely
without a reboot.

## Security

- **Admin-only**: uses the same admin check as `/restart` — only the first
  user in `TELEGRAM_CHAT_IDS` may use it.
- **Command prefix stripped**: the user writes `/at +CSQ` and the handler
  sends `AT+CSQ`; the `AT` prefix is injected by the handler, not the user,
  so the user cannot send a bare `AT` or introduce CR/LF injection.
- **Blacklisted destructive prefixes**: commands starting with `+CMGD`,
  `+CMGS`, `+CPBW`, `E`, `Z`, `&F` (factory reset) are rejected to prevent
  accidental data deletion or modem reset.
- **Timeout 5s**: the handler captures the full response and sends it back.

## Plan

1. Add `setAtCmdFn(std::function<String(const String &)>)` setter to
   `TelegramPoller`. Production wires this to a lambda that calls
   `realModem.sendAT(cmd)` + `waitResponse(5000, out)` and returns `out`.

2. Add `/at <cmd>` handler in `TelegramPoller::processUpdate`:
   - Parse the argument.
   - If no argument, show usage.
   - Strip leading `+` if present (some users will type `/at +CSQ`, others
     `/at AT+CSQ` — normalize to the bare form and let RealModem prepend `AT`).
     Actually: strip a leading `AT` or `at` prefix if present (user types
     `/at AT+CSQ`) since `sendAT` adds `AT` itself. Keep the `+` for standard
     commands like `+CSQ`.
   - Check blacklisted prefixes.
   - Call `atCmdFn_(cmd)` and reply with the result (truncated to 500 chars).

3. Add native tests.

4. Update `/help` and `telegram.cpp`.

## Notes for handover

`setAtCmdFn` takes a function that also receives the calling user ID so the
production lambda can perform the admin check inline. Alternatively the admin
check happens in the handler using a separate `isAdminFn`. For simplicity,
the handler accepts a `ListMutatorFn`-style pattern: take `fromId` + `cmd`,
return the response String (empty = not authorized).

Actually simpler: use the existing `AdminCheckFn` pattern. The `mutator_` lambda
already handles admin checks for `/restart` — but that takes a different
signature. Best approach: add `atCmdFn_` as a `std::function<String(int64_t
fromId, const String &cmd)>` — the lambda checks `fromId == adminId` and
returns an error string if not authorized.
