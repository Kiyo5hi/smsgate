---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0216: `/retry <N>` — force immediate retry of a single queue entry

## Motivation

`/flushqueue` resets all retry timers at once. When one entry is
stuck (e.g. the recipient number is temporarily busy) but the others
should keep their backoff schedule, `/retry N` lets the operator target
just that entry.

## Design

New command: `/retry <N>` (1-based, matching `/queue` display)

Resets the `nextRetryMs` of the Nth occupied queue entry to 0 so
`drainQueue()` attempts it on the next tick. All other entries are
unaffected.

On success:
```
🔄 Entry 2 will retry on next tick.
```

On invalid N:
```
❌ No entry 2 in queue (1 total).
```

## Notes for handover

- Adds `resetRetryTimer(int n)` to `SmsSender` (1-based, matching
  `/cancel` naming convention). Returns true if found.
- No new NVS, no new interface changes.
- 1 native test: enqueue 2 entries; retry entry 1; assert entry 1
  nextRetryMs == 0 and entry 2 unchanged.
