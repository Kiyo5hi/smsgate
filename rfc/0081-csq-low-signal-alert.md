---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0081: CSQ low-signal alert

## Motivation

When modem signal drops to CSQ ≤ 5 (mapped to ≤ −107 dBm by 3GPP),
SMS delivery and Telegram connectivity both degrade. The operator
currently has no notification unless they manually run `/status`. A
proactive alert mirrors the low-heap warning pattern from RFC-0066 and
lets the operator investigate physical placement issues before messages
start failing.

Thresholds (matching `csqLabel` logic already in statusFn):
- Alert: CSQ ≤ 5 and alert not already sent → send "📶 Low signal: CSQ N"
- Clear: CSQ > 10 → clear the alert flag (hysteresis gap prevents flapping)

## Plan

**`src/main.cpp`**:
- Add `static bool s_lowCsqWarnSent = false;` near `s_lowHeapWarnSent`.
- In the 30-second block, after the CSQ refresh (cachedCsq is now updated):
  ```cpp
  if (cachedCsq > 0 && cachedCsq <= 5 && !s_lowCsqWarnSent) {
      realBot.sendMessage("📶 Low signal: CSQ " + String(cachedCsq));
      s_lowCsqWarnSent = true;
  } else if (cachedCsq > 10) {
      s_lowCsqWarnSent = false;
  }
  ```
  CSQ == 0 is treated as "unknown" (modem not yet queried) — skip it.

## Notes for handover

Changed: `src/main.cpp`, `rfc/0081-csq-low-signal-alert.md`.
