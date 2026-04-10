---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0221: /schedrepeat <N> <min> — recurring scheduled SMS

## Motivation

All scheduled SMS are one-shot today. A periodic check-in message (e.g. "I'm OK" every
day, or a service heartbeat SMS every hour) requires the operator to re-issue
`/schedulesend` after each fire. `/schedrepeat` marks a slot to automatically
reschedule itself after each successful delivery.

## Current state

`ScheduledSms` has `sendAtMs`, `phone`, `body`. On fire the slot is cleared.
The NVS blob is version 0x01: 1 byte + 5 × 164-byte slots (`uint32_t sendAtUnix`,
`char phone[32]`, `char body[128]`).

## Plan

1. Add `uint32_t repeatIntervalMs = 0` to `ScheduledSms` (0 = one-shot).
2. `/schedrepeat <N> <min>` — set repeat interval for slot N (1-based).
   - 0 min = convert to one-shot. Max 10080 min (7 days).
   - Calls `persistSchedFn_`.
3. `tick()` fire path: if `repeatIntervalMs > 0`, reset `sendAtMs = nowMs + repeatIntervalMs`
   instead of clearing the slot.
4. `/schedqueue` display: append "🔁 Xm" suffix on repeating slots.
5. `/schedinfo <N>`: show repeat interval.
6. Help entry.
7. NVS blob bumped to version 0x02: 5 × 168-byte slots (add `uint32_t repeatIntervalSec`
   per slot, stored as seconds to match sendAtUnix units). Old 0x01 blobs are silently
   dropped (the version check fails).

## Notes for handover

Slot persistence in main.cpp uses a version byte to detect format changes; bumping to
0x02 is the intended upgrade path. One-shot schedules survive reboot; repeat schedules
also survive since repeatIntervalSec is serialized.
