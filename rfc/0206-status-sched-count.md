---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0206: Show scheduled SMS count in /status

## Motivation

`/status` shows the outbound retry queue depth but not the scheduled SMS
queue. A user with pending scheduled messages has no quick way to see them
without running `/schedqueue`. Adding a "Sched: N/5 pending" line to the
SMS section closes the gap — the number is visible alongside other queue
metrics at a glance.

## Design

In the `/status` lambda in `main.cpp`, add after the "Outbound queue" line:

```cpp
// RFC-0206: Scheduled SMS count.
if (telegramPoller) {
    int schedPending = 0;
    const auto& sq = telegramPoller->getSchedQueue();
    for (size_t i = 0; i < sq.size(); i++)
        if (sq[i].sendAtMs != 0) schedPending++;
    if (schedPending > 0) {
        msg += "  Sched: ";
        msg += String(schedPending);
        msg += "/";
        msg += String((int)TelegramPoller::kScheduledQueueSize);
        msg += " pending\n";
    }
}
```

Only show the line when `schedPending > 0` to avoid noise on most boots.

## Notes for handover

- No new setter needed; uses existing `getSchedQueue()` (RFC-0200) and
  the public `kScheduledQueueSize` constant (promoted to public in RFC-0200).
- The status lambda accesses `telegramPoller` as a file-scope static,
  which is non-null by the time `/status` is called.
