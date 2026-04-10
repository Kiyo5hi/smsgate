---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0105 — /sim command

## Motivation

`/status` shows ICCID, IMEI, operator, and signal quality scattered through a
long message. Sometimes the user just needs to confirm which SIM is active or
check the ICCID after a SIM swap. A dedicated `/sim` command provides a compact
snapshot of SIM identity fields without scrolling through the full status.

## Plan

1. Add a `setSimInfoFn(std::function<String()>)` setter to `TelegramPoller`.
   Production wires this to a lambda that reads the file-scope cached values
   `cachedImei`, `cachedIccid`, `cachedOperatorName`, and `cachedCsq` from
   `main.cpp`.

2. Add a `/sim` handler in `TelegramPoller::processUpdate` that calls
   `simInfoFn_()` and sends the result. Falls back to "(SIM info not
   configured)" when the setter was never called.

3. The response format:
   ```
   📶 SIM info
     ICCID: 89860xxxxxxxxxxxxxxxxx
     IMEI: 8613xxxxxxxxxxxxxxx
     Operator: China Mobile
     CSQ: 14 (ok)
   ```

4. Update `/help` and `telegram.cpp` registered commands.

5. Add native tests for the handler.

## Notes for handover

The `/csq` command already shows signal + operator. `/sim` adds the SIM
identity fields (ICCID, IMEI) that `/csq` doesn't show, making them
complementary: `/csq` for connectivity health, `/sim` for identity/audit.
