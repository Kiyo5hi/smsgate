---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0075: Daily Telegram stats digest

## Motivation

When the bridge is running normally there is no Telegram traffic for
hours or days. The operator has no passive signal that the device is
alive and healthy. A daily digest (sent once per 24 hours) gives a
low-noise heartbeat: it confirms liveness and summarises SMS activity
without requiring the operator to manually query `/status`.

## Plan

**`src/main.cpp`**:
- Add `static uint32_t s_lastDailyDigestMs = 0;` near the other
  30-second-block statics.
- In the 30-second periodic block (alongside the heap and SIM checks),
  add a 24-hour timer:
  ```
  if (millis() - s_lastDailyDigestMs >= 24UL * 3600UL * 1000UL) {
      s_lastDailyDigestMs = millis();
      // build and send digest
  }
  ```
  Skip on the very first iteration (s_lastDailyDigestMs == 0 → set and
  skip) to avoid sending immediately at boot.
- Digest content (compact, one-liner style):
  ```
  📊 24 h digest | fwd N (session) M (lifetime) | blocked N | deduped N | heap N KB free
  ```

## Notes for handover

Changed: `src/main.cpp`, `rfc/0075-daily-stats-digest.md`.

No new tests — the 30-second periodic block is not covered by the
native suite.
