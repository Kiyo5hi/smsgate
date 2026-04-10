---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0138: /setmaxfail command — change consecutive-failure reboot threshold

## Motivation

`SmsHandler::MAX_CONSECUTIVE_FAILURES = 8` is compile-time. In low-signal
conditions the device reboots aggressively; in stable conditions it's
fine to raise the threshold. Runtime control avoids reflashing.

## Plan

- Change `MAX_CONSECUTIVE_FAILURES` from `static constexpr int` to a
  runtime field `maxConsecutiveFailures_` in `SmsHandler`, defaulting to 8.
- Add `void setMaxConsecutiveFailures(int n)` to `SmsHandler`.
- Add `setMaxFailFn(std::function<void(int)>)` setter to `TelegramPoller`.
- Command `/setmaxfail <N>` (N = 0 disables reboot, max 99).
- In `main.cpp` wire: lambda calls `smsHandler.setMaxConsecutiveFailures(n)`.
  No NVS persist needed (reboot resets to 8, which is the safe default).

## Notes for handover

Tests that reference `SmsHandler::MAX_CONSECUTIVE_FAILURES` by name must
be updated to use the instance accessor `smsHandler.maxConsecutiveFailures()`.
