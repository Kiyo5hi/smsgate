---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0269: Snooze / mute / fwd-pause expiry checks are not millis()-wraparound-safe

## Motivation

Six `<` / `>=` comparisons against millis()-derived timestamps use the
naive form rather than the RFC-0266 wraparound-safe idiom.  They all
share the same failure mode:

If the device runs for ~49.7 days (millis() near UINT32_MAX) and a
snooze/mute/pause is set whose expiry timestamp overflows uint32_t into
a small value, then after the wrap:

- `now >= expiry` evaluates **false** (large `now` is not >= small
  `expiry`), so the expiry is never detected.
- `now < expiry` evaluates **false** likewise, so an active snooze
  appears inactive.

### Affected sites

| File | Line | Expression | Effect of bug |
|------|------|-----------|---------------|
| `telegram_poller.h` | 421 | `now >= it->second` | `isSnoozed()` — snooze never expires; SMS from that phone permanently suppressed until reboot |
| `telegram_poller.cpp` | 1192 | `piNow < s.second` | `/phoneinfo` shows "not snoozed" for an active snooze |
| `telegram_poller.cpp` | 4014 | `nowMs >= it->second` | `/snoozelist` reap misses expired entry |
| `telegram_poller.cpp` | 4522 | `now >= it->second` | `tick()` reap misses expired entry |
| `main.cpp` | 234 | `millis() < s_alertsMutedUntilMs` | Alert mute never expires; all proactive alerts silenced until reboot |
| `main.cpp` | 2436 | `millis() >= s_fwdPauseUntilMs` | Forward-pause never auto-resumes; all SMS forwarding off until reboot |

The most severe consequences are in `isSnoozed()` (silent SMS loss) and
`alertsMuted()` / fwd-pause (silent suppression of all alerts or all
forwarding).

## Plan

Replace each comparison with the RFC-0266 signed-subtraction idiom
`(uint32_t)(a - b) < 0x80000000UL`.  The idiom is safe for intervals
≤ 2^31 ms ≈ 24.8 days.  Maximum snooze/mute/pause durations are
measured in minutes to hours — well within the safety window.

### telegram_poller.h:421 (isSnoozed — has expiry passed?)

```cpp
// Before:
if (now >= it->second) { snoozeList_.erase(it); return false; }

// After:
if ((uint32_t)(now - it->second) < 0x80000000UL) { snoozeList_.erase(it); return false; }
```

### telegram_poller.cpp:1192 (/phoneinfo — is snooze still active?)

```cpp
// Before:
if (s.first == piPhone && piNow < s.second)

// After:
if (s.first == piPhone && (uint32_t)(s.second - piNow) < 0x80000000UL)
```

### telegram_poller.cpp:4014 (/snoozelist reap — has expiry passed?)

```cpp
// Before:
if (nowMs >= it->second)

// After:
if ((uint32_t)(nowMs - it->second) < 0x80000000UL)
```

### telegram_poller.cpp:4522 (tick() reap — has expiry passed?)

```cpp
// Before:
if (now >= it->second)

// After:
if ((uint32_t)(now - it->second) < 0x80000000UL)
```

### main.cpp:234 (alertsMuted — is mute still active?)

```cpp
// Before:
inline bool alertsMuted() { return (uint32_t)millis() < s_alertsMutedUntilMs; }

// After:
inline bool alertsMuted() {
    return s_alertsMutedUntilMs != 0 &&
           (uint32_t)(s_alertsMutedUntilMs - (uint32_t)millis()) < 0x80000000UL;
}
```

The explicit `!= 0` guard makes the "not muted" sentinel (0) unambiguous.

### main.cpp:2436 (fwd-pause auto-resume — has expiry passed?)

```cpp
// Before:
if (s_fwdPauseUntilMs != 0 && (uint32_t)millis() >= s_fwdPauseUntilMs)

// After:
if (s_fwdPauseUntilMs != 0 &&
    (uint32_t)((uint32_t)millis() - s_fwdPauseUntilMs) < 0x80000000UL)
```

## Notes for handover

- No schema change; no NVS migration needed.
- All six changes are one-liners.
- The `0x80000000UL` idiom is identical to what RFC-0266 introduced in
  `TelegramPoller::tick()` for the scheduler fire check.
