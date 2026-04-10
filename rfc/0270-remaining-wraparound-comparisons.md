---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0270: Remaining millis()-wraparound-unsafe comparisons

## Motivation

Follow-up to RFC-0269.  Three more unsafe comparisons were found
involving millis()-based timestamps, covering the retry gate, the
stuck-queue alert, and the LRU eviction for buffered concat groups.

### Bug 1 — SmsSender retry gate (sms_sender.cpp:164)

```cpp
if (nowMs < e.nextRetryMs)
    continue;
```

When `nowMs + kBackoffMs` overflows uint32_t (only in the last 16
seconds before the ~49.7-day wrap), `nextRetryMs` wraps to a small
value.  If `nowMs` is still large (just before the wrap), the comparison
`large < small` is FALSE, so the retry fires immediately instead of
waiting the backoff interval.  One extra premature retry attempt.

### Bug 2 — Stuck-queue alert guard (main.cpp:2797)

```cpp
if (e.queuedAtMs > 0 && nowMs2 >= e.queuedAtMs &&
    (nowMs2 - e.queuedAtMs) >= kStuckQueueThresholdMs)
```

The `nowMs2 >= e.queuedAtMs` guard is meant to prevent showing a
negative age but it fires incorrectly when the device wraps:
`small >= large` → FALSE, so the stuck alert is silenced for entries
queued in the ~5 minutes before the wrap.

### Bug 3 — LRU eviction for concat groups (sms_handler.cpp:106,237,268)

```cpp
if (concatGroups_[i].lastSeenMs < concatGroups_[lruIdx].lastSeenMs)
```

If two groups were last seen on opposite sides of the uint32_t wrap,
the comparison inverts and the NEWEST group is evicted instead of the
oldest.  This discards in-progress SMS concat assembly for a recently
active group while keeping a stale one.

## Plan

### sms_sender.cpp:164

Replace with the wraparound-safe "now >= target" idiom, preserving
sentinel 0 = "retry now":

```cpp
// Before:
if (nowMs < e.nextRetryMs)
    continue;

// After (skip while now < target, i.e. (now-target) is a large wrapped uint):
if (e.nextRetryMs != 0 && (uint32_t)(nowMs - e.nextRetryMs) >= 0x80000000UL)
    continue;
```

Note the direction: `(uint32_t)(now - target) < 0x80000000UL` means
"now >= target" (fire); the skip condition is the inverse `>= 0x80000000`.

### main.cpp:2797–2798

Drop the unsafe `nowMs2 >= e.queuedAtMs` guard and rely solely on
unsigned subtraction (which is inherently wraparound-safe):

```cpp
// Before:
if (e.queuedAtMs > 0 && nowMs2 >= e.queuedAtMs &&
    (nowMs2 - e.queuedAtMs) >= kStuckQueueThresholdMs)

// After:
if (e.queuedAtMs > 0 &&
    (uint32_t)(nowMs2 - e.queuedAtMs) >= kStuckQueueThresholdMs)
```

### sms_handler.cpp:106

In `evictLruUntilUnderCaps()`, add a snapshot of `clock_()` and use
elapsed-time comparison:

```cpp
uint32_t evictNow = (uint32_t)(clock_ ? clock_() : 0);
// Then:
if ((uint32_t)(evictNow - concatGroups_[i].lastSeenMs) >
    (uint32_t)(evictNow - concatGroups_[lruIdx].lastSeenMs))
```

### sms_handler.cpp:237,268

`now` (from `clock_()`) is already in scope at these two inline LRU
loops.  Apply the same elapsed-time comparison.

## Notes for handover

- No schema change; no NVS migration needed.
- All bugs require ~49.7 days uptime to trigger.
- Bug 1: one extra retry attempt (harmless in practice).
- Bug 2: 5-minute blind spot for stuck-queue detection near the wrap.
- Bug 3: wrong concat group evicted; could drop in-flight multi-part SMS.
