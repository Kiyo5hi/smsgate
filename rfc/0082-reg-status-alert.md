---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0082: Network registration loss / recovery alert

## Motivation

When the modem loses network registration (e.g., SIM ejected, APN
misconfiguration, carrier outage) incoming SMS silently disappear.
The operator sees no Telegram traffic but doesn't know whether the
bridge is healthy or has lost cellular connectivity. An alert on
registration loss + recovery gives an early warning with no manual
/status polling needed.

## Plan

States: `REG_OK_HOME` or `REG_OK_ROAMING` = registered. All other
states = not registered.

**`src/main.cpp`**:
- Add `static bool s_regLostAlertSent = false;`.
- After updating `cachedRegStatus` in the 30-second block:
  ```cpp
  bool regOk = (cachedRegStatus == REG_OK_HOME || cachedRegStatus == REG_OK_ROAMING);
  if (!regOk && !s_regLostAlertSent && cachedCsq > 0) {
      realBot.sendMessage("📵 Network registration lost (" + regStr + ")");
      s_regLostAlertSent = true;
  } else if (regOk && s_regLostAlertSent) {
      realBot.sendMessage("✅ Network registration restored (" + regStr + ")");
      s_regLostAlertSent = false;
  }
  ```
  Guard on `cachedCsq > 0` to avoid false alerts on the first 30-second
  tick before the modem has had a chance to respond.

## Notes for handover

Changed: `src/main.cpp`, `rfc/0082-reg-status-alert.md`.
