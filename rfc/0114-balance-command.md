---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0114 — /balance command

## Motivation

RFC-0106 appends the carrier balance to the scheduled heartbeat when
`USSD_BALANCE_CODE` is defined. But operators often want an on-demand
balance check without waiting for the next heartbeat. The `/ussd`
command works (`/ussd *100#`), but requires typing the balance code.
A dedicated `/balance` command is a one-tap shortcut.

## Plan

1. Add a `setBalanceCodeFn(std::function<String()> fn)` setter to
   `TelegramPoller`. The fn returns the configured balance USSD code
   (or an empty string if the code is not set). Production wires this
   to a lambda returning the `USSD_BALANCE_CODE` define (or `""` when
   the define is absent).

2. In `doHandleMessage`, add a `/balance` case:
   - If `balanceCodeFn_` is null or returns empty → reply
     "Balance check not configured (define USSD_BALANCE_CODE)."
   - Otherwise call `ussdFn_(code)` (reusing the existing USSD fn) and
     reply with the result (or "No response from carrier." on empty).

3. Wire in `main.cpp`:
   ```cpp
   poller.setBalanceCodeFn([]() -> String {
   #ifdef USSD_BALANCE_CODE
       return String(USSD_BALANCE_CODE);
   #else
       return String();
   #endif
   });
   ```

4. Register `/balance` in `setMyCommands`.

5. Tests:
   - `/balance` with both fns set → replies with USSD result.
   - `/balance` with no code fn → "not configured" reply.
   - `/balance` with code fn returning empty → "not configured" reply.
   - `/balance` when USSD returns empty → "No response" reply.

## Notes for handover

`balanceCodeFn_` decouples the USSD code from TelegramPoller so the
poller stays free of `#ifdef`. The USSD call itself is routed through
the existing `ussdFn_`, so there's no new modem interaction needed.
