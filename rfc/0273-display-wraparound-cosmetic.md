---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0273: Remaining display-only millis()-wraparound cosmetic fixes

## Motivation

Follow-up to RFC-0270.  Three display-only comparisons in
`telegram_poller.cpp` use unsafe timestamp arithmetic that produces
wrong output (not wrong behavior) near the uint32_t wrap at ~49.7 days
uptime.  None of these affect SMS send/receive correctness.

### Bug 1 — `schedEtaStr()` overdue check (line 23)

```cpp
if (nowMs >= sendAtMs) { return String("now (overdue)"); }
```

A scheduled-SMS slot whose `sendAtMs` just crossed the uint32_t wrap
would show "now (overdue)" on the status display when it is actually
still in the future.

### Bug 2 — `/queueinfo` next-retry display (line ~2053)

```cpp
if (e.nextRetryMs > 0 && nowMs < e.nextRetryMs)
```

Same direction as the old `sms_sender.cpp` bug (RFC-0270): at exact
equality or near wrap, `nowMs < e.nextRetryMs` is false when it should
be true, so the display shows "now" instead of the actual remaining
seconds.

### Bug 3 — `/schedexport` signed-cast subtraction (lines ~3238, ~3253)

```cpp
long deltaMs = (long)sl.sendAtMs - (long)expNowMs;
```

Casting both sides to `long` (32-bit on Xtensa) loses the wrap-safe
property of unsigned subtraction.  For a slot scheduled past the 32-bit
wrap, the signed result is negative, so the exported `/scheduleat` or
`/schedulesend` command has a wrong (past) timestamp or the minimum 1m
fallback.

## Plan

### `schedEtaStr()` line 23

```cpp
// Before:
if (nowMs >= sendAtMs) { return String("now (overdue)"); }
// After:
if ((uint32_t)(nowMs - sendAtMs) < 0x80000000UL) { return String("now (overdue)"); }
```

### `/queueinfo` line ~2053

```cpp
// Before:
if (e.nextRetryMs > 0 && nowMs < e.nextRetryMs)
// After:
if (e.nextRetryMs > 0 && (uint32_t)(nowMs - e.nextRetryMs) >= 0x80000000UL)
```

(Skip condition: `>=` means now < target, same idiom as RFC-0270.)

### `/schedexport` lines ~3238, ~3253

Replace signed-cast subtraction with unsigned, then bound-check:

```cpp
// Before:
long deltaMs = (long)sl.sendAtMs - (long)expNowMs;
// After:
uint32_t diffU = sl.sendAtMs - expNowMs;
long deltaMs = (diffU < 0x80000000UL) ? (long)diffU : 0L;
```

The `0L` fallback treats genuinely overdue slots as "fire now", which
the surrounding `<= expWallNow` / `< 1` guards then convert to the
same +1min / +1m floor they already apply.

## Notes for handover

- All three are display-only; no send/receive logic is affected.
- Requires ~49.7-day uptime to manifest.
- No new state, no NVS migration needed.
