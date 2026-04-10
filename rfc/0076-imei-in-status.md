---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0076: IMEI in /status

## Motivation

In multi-device deployments (or when sharing a bot token across test
and production devices) the operator needs a way to distinguish which
physical device is responding. The IMEI is the canonical, unchanging
modem identity and is available via `modem.getIMEI()` (TinyGSM wrapper
for `AT+GSN`).

## Plan

**`src/main.cpp`**:
- Add `static String cachedImei;` alongside `cachedModemFirmware`.
- In setup(), after the RFC-0045 modem-firmware query block, add:
  ```cpp
  cachedImei = modem.getIMEI(); cachedImei.trim();
  ```
- In the statusFn lambda, after the modem FW line, add:
  ```cpp
  if (cachedImei.length() > 0) { msg += "\n  IMEI: "; msg += cachedImei; }
  ```

## Notes for handover

Changed: `src/main.cpp`, `rfc/0076-imei-in-status.md`.

No new tests — IMEI is a modem read-only property not exercised in
the native suite.
