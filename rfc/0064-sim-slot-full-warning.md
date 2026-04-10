---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0064: SIM slot full warning

## Motivation

If the SIM storage fills up the modem rejects new incoming SMS silently.
A proactive alert when usage crosses ≥80% gives the user time to act before
messages are lost.

## Plan

**`src/main.cpp`**:
- File-scope static `static bool s_simFullWarnSent = false;`
- After the 30s AT+CPMS? refresh block: check `cachedSimUsed * 5 >=
  cachedSimTotal * 4` (≥80%). If true and the warning hasn't been sent yet,
  send `"⚠️ SIM storage N/M slots (≥80%)..."` and set the flag. Clear the
  flag when usage drops below threshold so a re-fill re-alerts.

## Notes for handover

Only `src/main.cpp` changed. No test changes needed.
