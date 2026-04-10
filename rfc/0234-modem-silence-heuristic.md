---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0234: Activity-gated modem health check

## Motivation

RFC-0229 added a periodic modem `testAT()` check every 5 minutes.
Two problems:
1. Healthy modems emit URCs (registration updates, CSQ queries, etc.)
   regularly. Calling `testAT()` every 5 minutes when the modem is
   actively sending data is wasteful and risks swallowing a +CMTI URC
   in TinyGSM's internal `waitResponse` window.
2. 5-minute checks with a streak of 3 means 15 minutes to detect a hung
   modem — too slow.

## Plan

1. Track `s_lastSerialActivityMs` — updated on every non-empty line
   received in the SerialAT drain loop.
2. Gate the health check: only fire `testAT()` when there has been no
   serial activity for at least `kModemSilenceThresholdMs` = 2 minutes.
   If the modem is actively sending URCs, the check is skipped entirely
   and the fail streak is reset to 0.
3. Reduce the check interval to 2 minutes (from 5).
4. Reduce the fail streak to 2 (from 3) → total time to reboot on hung
   modem: ~4 minutes (2 min silence before first check + 2 min between
   checks) vs. 15 minutes before.

## Notes for handover

The silence heuristic avoids calling `testAT()` while the modem is busy,
which prevents the URC-swallowing concern. A truly hung modem will be
silent, so the check correctly fires only when silence indicates trouble.
