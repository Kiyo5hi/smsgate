---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0210: Add scheduled SMS count to heartbeat one-liner

## Motivation

RFC-0206 added the scheduled SMS count to `/status`. The heartbeat
one-liner is the other place operators look — it should also surface
occupied sched slots so an inadvertently-left-running scheduled SMS
is visible without polling.

## Design

In the heartbeat block in `main.cpp`, after the `| q N/8` field,
append `| sched N` only when at least one slot is occupied:

```
⏱ 0d 6h 0m | CSQ 18 | WiFi -62dBm | fwd 42 | calls 1 | q 0/8 | sched 1
```

Count occupied slots inline:

```cpp
int schedCount = 0;
const auto& sq = telegramPoller->getSchedQueue();
for (const auto& slot : sq)
    if (slot.sendAtMs != 0) schedCount++;
if (schedCount > 0) {
    hb += String(" | sched "); hb += String(schedCount);
}
```

## Notes for handover

- Conditional display (`> 0` only) keeps the heartbeat terse when
  nothing is scheduled — same approach as RFC-0206.
- No new APIs. Uses `telegramPoller->getSchedQueue()` (already public).
- No native test needed: the heartbeat is in `main.cpp` (non-testable
  from native env). Build test is sufficient.
