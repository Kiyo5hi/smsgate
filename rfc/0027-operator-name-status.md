---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0027: Modem operator name in `/status`

## Motivation

The `/status` modem line shows CSQ and registration state but not which
carrier the SIM is registered to. `AT+COPS?` returns the operator name
string (e.g. "China Unicom"). Useful for roaming diagnosis and SIM swap
verification.

## Plan

- Add `static String cachedOperatorName` alongside `cachedCsq`/`cachedRegStatus`.
- Parse `AT+COPS?` response (first quoted token) in the 30s loop() refresh
  block and at boot (before the startup banner).
- Show in `statusFn` as `"home (China Unicom)"` — empty name is silently
  omitted.

## Notes for handover

Only `src/main.cpp` changed. No interface changes, no new tests needed
(excluded from native build).
