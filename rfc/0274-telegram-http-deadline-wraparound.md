---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0274: Wraparound-unsafe millis() deadline checks in telegram.cpp and main.cpp

## Motivation

Follow-up to RFC-0273.  Several short-lived HTTP read deadlines in
`telegram.cpp` (4 s body drain, 5 s header drain, long-poll body
drain) and the 30 s NTP deadline in `main.cpp` use the naive
`millis() < deadline` / `millis() > deadline` comparison.

If the uint32_t `millis()` counter wraps during one of these windows:

- `millis() < deadline` — where deadline wrapped to a small value —
  evaluates FALSE immediately, so the read loop exits prematurely.
  This cuts the body drain short, leaving stale bytes in the TLS
  buffer.  The CLAUDE.md "do not early-break" invariant identifies
  incomplete drains as a real bug that corrupts subsequent requests.
- `millis() > deadline` — header timeout guard — fires immediately
  when `millis()` is large and `deadline` wrapped small, causing a
  spurious `transport_->stop()` mid-header.

## Locations fixed

### telegram.cpp — sendBotMessage body drain (line ~666)
```cpp
while (body.length() < target && millis() < deadline)
→ while (body.length() < target && (uint32_t)(deadline - (uint32_t)millis()) < 0x80000000UL)
```

### telegram.cpp — doSendMessage header deadline (line ~765)
```cpp
if (millis() > headerDeadline)
→ if ((uint32_t)((uint32_t)millis() - headerDeadline) < 0x80000000UL)
```

### telegram.cpp — doSendMessage body drain (lines ~789, ~804)
```cpp
while (...&& millis() < deadline) / if (millis() >= deadline ...)
→ wraparound-safe idioms
```

### telegram.cpp — getUpdates header + body drain (lines ~938, ~959, ~974)
```cpp
if (millis() > readDeadline) / while (...&& millis() < readDeadline) /
if (millis() >= readDeadline ...)
→ wraparound-safe idioms
```

### main.cpp — syncTime() NTP deadline (line ~305)
```cpp
while (... && millis() < ntpDeadline)
→ while (... && (uint32_t)(ntpDeadline - (uint32_t)millis()) < 0x80000000UL)
```

## Idiom summary

| Intent | Safe expression |
|--------|----------------|
| "deadline still in future" (keep looping) | `(uint32_t)(deadline - (uint32_t)millis()) < 0x80000000UL` |
| "deadline has passed" (fire/stop) | `(uint32_t)((uint32_t)millis() - deadline) < 0x80000000UL` |

## Notes

- All windows are short (4–30 s), so the bug requires the 49.7-day
  wrap to land within those exact windows. Probability is very low,
  but the consequence (corrupted TLS stream) is severe and hard to
  diagnose.
- No behavior change for normal operation; only the ~49.7-day edge
  case is fixed.
