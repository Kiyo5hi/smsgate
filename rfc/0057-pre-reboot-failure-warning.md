---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0057: Pre-reboot warning on consecutive Telegram failure threshold

## Motivation

When `SmsHandler` reaches `MAX_CONSECUTIVE_FAILURES` (8) it reboots without
any user-visible warning. If Telegram is temporarily reachable for one message
between failure 7 and failure 8, the user gets no indication the bridge is
about to restart. A warning on failure 7 gives the user visibility.

## Plan

**`src/sms_handler.cpp`** — in `noteTelegramFailure()`, after incrementing,
add a warning attempt at `MAX_CONSECUTIVE_FAILURES - 1`:
```cpp
if (consecutiveFailures_ == MAX_CONSECUTIVE_FAILURES - 1) {
    bot_.sendMessage("⚠️ 7/8 consecutive Telegram failures — bridge will reboot on next failure.");
}
```

The send is best-effort: if Telegram is unreachable (the likely cause of the
failures) the warning is lost, but that's acceptable.

## Notes for handover

Only `src/sms_handler.cpp` changed. No test changes needed — the failure path
is already exercised by existing tests; the warning send doesn't affect control
flow.
