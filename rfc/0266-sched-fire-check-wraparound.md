---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0266: Scheduler fire-check is not millis()-wraparound-safe

## Motivation

The scheduler tick fires a slot when:

```cpp
uint32_t now = clock_();
if (slot.sendAtMs != 0 && now >= slot.sendAtMs && !inQuiet)
```

`uint32_t` `millis()` wraps at 2^32 ms ≈ 49.7 days.

When scheduling a recurring slot that will fire **after the wraparound
point**, `sendAtMs` is computed as:

```cpp
slot.sendAtMs = now + slot.repeatIntervalMs;
```

If `now` is near UINT32_MAX (e.g., day 48) and `repeatIntervalMs` is
7 days (max allowed: `10080 min × 60 000 = 604 800 000 ms`), the sum
overflows:

```
now = 4 147 200 000   (day 48)
now + 604 800 000 = 4 752 000 000 → overflows to 457 032 704
```

On the very next `tick()`, `now (4 147 200 000) >= sendAtMs (457 032 704)`
is **true** and the slot fires immediately.  The loop continues to fire on
every subsequent tick until `sendAtMs` wraps past `now` again.

## Impact

* Only affects devices with **≥ 43 days of continuous uptime** that have
  an active recurring slot with an interval > ~6.7 days.
* Consequence: recurring SMS fires early / in a burst rather than on
  schedule.  No crash, no data loss.

## Plan

Replace the comparison with the standard wraparound-safe idiom:

```cpp
// Before
now >= slot.sendAtMs

// After: fires when (now - sendAtMs) ≥ 0 in signed int32 sense,
// but using unsigned subtraction which wraps correctly.
// Safe for intervals ≤ 24.8 days (half uint32_t range).
(uint32_t)(now - slot.sendAtMs) < 0x80000000UL
```

This is equivalent to `(int32_t)(now - slot.sendAtMs) >= 0` and handles
all four combinations of pre/post-wraparound `now` and `sendAtMs` correctly
as long as the interval is ≤ 2^31 ms ≈ 24.8 days.  Our maximum interval is
10080 min = 7 days < 24.8 days, so this is safe.

## Notes for handover

- One-line change in `TelegramPoller::tick()`.
- No schema change; no NVS migration needed.
- The same fix applies to the `sendAtMs != 1` sentinel (used for past-due
  slots at boot): `sendAtMs == 1` means fire immediately; the subtraction
  approach handles it correctly because `(uint32_t)(now - 1) < 0x80000000`
  is true for any reasonable `now`.
