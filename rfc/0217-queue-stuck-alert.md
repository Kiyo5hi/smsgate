---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0217: Alert when outbound queue stays stuck for too long

## Motivation

If an outbound SMS has been in the retry queue for >30 minutes without
succeeding, the operator should know. Currently the only signal is the
heartbeat's `| q N/8` counter, which requires the operator to notice a
non-zero value. An automatic Telegram alert removes this blind spot.

## Design

In `TelegramPoller::tick()`, after the scheduled-fire drain, scan
SmsSender's queue snapshot for entries whose `queuedAtMs` is older than
`kQueueStuckThresholdMs` (30 minutes). If any such entry is found AND
the last "stuck alert" was more than `kQueueStuckAlertCooldownMs`
(60 minutes) ago, fire:

```
⚠️ Outbound queue stuck: 1 entry waiting >30min.
Oldest: +13800138000 (47 min). Use /queueinfo to inspect.
```

Cooldown prevents alert flooding. `lastQueueStuckAlertMs_` resets to 0
when the queue fully drains (detected via `smsSender_.queueSize() == 0`
after the queue was non-zero).

New members on TelegramPoller:
- `uint32_t lastQueueStuckAlertMs_ = 0`
- `bool queueWasNonEmpty_ = false`  (for drain-detection)

Constants (not configurable — keep it simple):
- `static constexpr uint32_t kQueueStuckThresholdMs  = 30 * 60 * 1000U`
- `static constexpr uint32_t kQueueStuckAlertCooldownMs = 60 * 60 * 1000U`

No new command, no NVS. Purely reactive tick() behaviour.

## Notes for handover

- Uses `clock_()` for nowMs and `smsSender_.getQueueSnapshot()`.
- Alert fires via `bot_.sendMessage(...)` to the admin chat (same as
  heartbeat and scheduled delivery confirmations).
- `queuedAtMs` is 0 until the first drain attempt; entries that have
  never been attempted are not counted as "stuck".
- 2 native tests: (a) alert fires after threshold; (b) cooldown
  suppresses repeat alert within window.
