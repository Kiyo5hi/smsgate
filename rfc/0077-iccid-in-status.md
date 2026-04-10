---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0077: SIM ICCID in /status

## Motivation

When troubleshooting SIM-related issues (wrong APN, quota exhausted,
SIM swap) the operator needs to confirm which physical SIM card is
inserted. The ICCID is the SIM's unique identifier, readable via
`AT+CICCID` (TinyGSM: `modem.getSimCCID()`).

## Plan

**`src/main.cpp`**:
- Add `static String cachedIccid;` alongside `cachedImei`.
- In setup(), after the RFC-0076 IMEI query block:
  ```cpp
  cachedIccid = modem.getSimCCID(); cachedIccid.trim();
  ```
- In the statusFn lambda, after the IMEI line:
  ```cpp
  if (cachedIccid.length() > 0) { msg += "\n  ICCID: "; msg += cachedIccid; }
  ```

## Notes for handover

Changed: `src/main.cpp`, `rfc/0077-iccid-in-status.md`.
