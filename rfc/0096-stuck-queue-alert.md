---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0096: Stuck-queue proactive alert

## Motivation

If the outbound SMS path breaks (modem not responding, SIM issue, etc.)
entries pile up in the queue but the operator only learns about it when
they check `/queue` or get a final-failure callback. A proactive alert
fires if any queue entry has been waiting > kStuckThresholdMs (5 minutes)
without delivering.

## Design

- In `main.cpp` `loop()`, add a periodic check (every 60 seconds) that
  calls `smsSender.getQueueSnapshot()` and looks for entries with
  `queuedAtMs > 0` and age > 5 minutes.
- On first detection, send a Telegram alert: "⚠️ Queue stuck: N entry(ies)
  waiting >5m. Oldest: <phone> (Ns)."
- Reset the "already alerted" flag when the queue becomes empty or after
  30 minutes (so repeated stalls re-alert).
- Don't alert during the first kStuckThresholdMs after boot (avoid false
  positives on startup drain).

## File changes

**`src/main.cpp`** — add stuck-queue alert logic in loop()
