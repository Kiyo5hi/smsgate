---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0267: /scheduleat overflow and RFC-0266 regression for long-dated slots

## Motivation

`/scheduleat` was introduced (RFC-0222) to allow scheduling SMS at an
absolute UTC date+time up to 365 days ahead. Two bugs interact:

### Bug 1 – int32 overflow in delay-to-milliseconds conversion

`long` on ESP32 (Xtensa LX6) is a 32-bit signed integer. The conversion:

```cpp
long delayMsAt = deltaSecAt > 0L ? deltaSecAt * 1000L : 1L;
```

overflows when `deltaSecAt > INT32_MAX / 1000 = 2,147,483` seconds
(≈ 24 days 21 h). The 365-day cap allows schedules up to 31,536,000
seconds, whose product `31,536,000,000` wraps to `1,471,228,928` ms
(≈ 17 days). The device silently fires the SMS ~17 days early.

### Bug 2 – RFC-0266 scheduler fix constrains slots to ≤ 24.8 days

RFC-0266 replaced the wraparound-unsafe comparison

```cpp
now >= slot.sendAtMs
```

with the signed-subtraction idiom

```cpp
(uint32_t)(now - slot.sendAtMs) < 0x80000000UL
```

This idiom is only correct when the scheduled interval is ≤ 2^31 ms
≈ 24.8 days (half the uint32_t range). For any slot where
`sendAtMs - nowMs > 0x80000000`, the expression evaluates to `true`
at creation time and the slot fires immediately.

Example: `nowMs = 500,000,000` ms, delay = 25 days = 2,160,000,000 ms:
```
sendAtMs = 500,000,000 + 2,160,000,000 = 2,660,000,000
(uint32_t)(500,000,000 − 2,660,000,000) = 2,134,967,296
2,134,967,296 < 2,147,483,648  → TRUE  → fires immediately ✗
```

### Combined effect

Any `/scheduleat` target > ~24.8 days in the future both:
* computes a wrong `sendAtAt` (overflow bug), AND
* fires immediately on the next `tick()` (RFC-0266 regression).

## Impact

Devices attempting to schedule SMS more than 24.8 days ahead via
`/scheduleat` will have the SMS fire immediately rather than on schedule.
No crash, no NVS corruption.

## Plan

Cap `/scheduleat` at **24 days** (`24 × 86400 = 2,073,600` seconds),
consistent with the scheduler's millis()-based 24.8-day safety window:

```cpp
// Before:
if (deltaSecAt > 365L * 86400L)
{
    bot_.sendMessageTo(u.chatId,
        String("\xe2\x9d\x8c Cannot schedule more than 365 days ahead.")); // ❌
    return;
}
```

```cpp
// After:
if (deltaSecAt > 24L * 86400L)
{
    bot_.sendMessageTo(u.chatId,
        String("\xe2\x9d\x8c Cannot schedule more than 24 days ahead"
               " (millis() scheduler limit).")); // ❌
    return;
}
```

With this cap:
* `deltaSecAt ≤ 2,073,600` s → `delayMsAt ≤ 2,073,600,000` ms < INT32_MAX ✓
* `sendAtMs − nowMs ≤ 2,073,600,000` ms < 0x80000000 (2,147,483,648) ✓

## Notes for handover

- One-line change in the `/scheduleat` block of `processUpdate()`.
- No schema change; no NVS migration needed.
- A long-range scheduling feature beyond 24 days would require storing
  the target as a Unix timestamp in the slot and comparing with
  `time(nullptr)` in `tick()`, not `millis()`. That is a separate RFC.
