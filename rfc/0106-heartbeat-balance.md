---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0106 — USSD balance in heartbeat

## Motivation

Prepaid SIMs run out of credit silently, causing the bridge to stop forwarding
SMS without any indication. If the user defines a USSD balance-check code for
their carrier (e.g. `*100#` for China Telecom), the periodic heartbeat can
automatically include the current balance, giving the operator advance notice
before the SIM goes dark.

## Plan

1. Add an optional `USSD_BALANCE_CODE` define to `secrets.h.example`.
   Example: `#define USSD_BALANCE_CODE "*100#"`.

2. In `main.cpp`, guard a new `setBalanceUssdFn` wiring under
   `#ifdef USSD_BALANCE_CODE`. The lambda calls `realModem.ussdQuery(...)`
   with a 10s timeout and caches the result in a file-scope `String
   s_cachedBalance`. Returns immediately (non-blocking) if the balance was
   queried recently (within the last heartbeat interval).

3. Actually simpler: in the heartbeat block in `loop()`, if
   `USSD_BALANCE_CODE` is defined, call `realModem.ussdQuery(...)` once and
   append the result to the heartbeat string as `| Bal: <text>`. No new
   setter needed.

4. Update `secrets.h.example` with the optional define documentation.

## Notes for handover

The USSD call in the heartbeat adds ~3–8s latency to the heartbeat send.
That's fine since the WDT is kicked before the heartbeat block and the
heartbeat interval is 6 hours by default. The balance string may be long
(e.g. "Your balance is 10.00 CNY. Data: 2.3GB.") — truncate at 40 chars
to keep the heartbeat message readable.
