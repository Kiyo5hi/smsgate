---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0192: `/pausefwd <minutes>` — temporary SMS forward pause

## Motivation

During maintenance windows (SIM swaps, modem firmware updates, local SMS
inspection) operators want to pause SMS forwarding for N minutes without
permanently disabling it. `/setforward off` + `/setforward on` works but
requires two commands and is easy to forget.

## Plan

### New main.cpp state

```cpp
static unsigned long s_fwdPauseUntilMs = 0; // 0 = not paused
```

Checked at the top of the existing `fwd_enabled` restoration block in
`setup()` (no change needed there — pause is volatile, not persisted).

In `loop()`, before the URC drain, add:

```cpp
if (s_fwdPauseUntilMs != 0 && millis() >= s_fwdPauseUntilMs)
{
    s_fwdPauseUntilMs = 0;
    smsHandler.setForwardingEnabled(true);
    Serial.println("Forward pause expired, forwarding re-enabled.");
}
```

### TelegramPoller command: `/pausefwd <minutes>`

- Range: 1–1440 (1 min to 24 hours).
- Calls `pauseFwdFn_(durationMs)` — the fn sets `smsHandler.setForwardingEnabled(false)`,
  sets `s_fwdPauseUntilMs = millis() + ms`, and returns a formatted
  "paused until HH:MM" or "paused for N min" string for the reply.
- Replies: "⏸ Forwarding paused for 30 min."
- `/setforward on` clears the pause (sets `s_fwdPauseUntilMs = 0`).
  The existing `setForwardingEnabledFn` lambda is wired to also clear
  `s_fwdPauseUntilMs` when `enable=true` is passed.
- Setter: `setPauseFwdFn(std::function<String(uint32_t durationMs)>)`.

### Persistence

None — pause is volatile. A reboot clears it (intentional: reboot = reset
state, NVS-persisted `fwd_enabled=1` will re-enable forwarding).

## Notes for handover

- The `/pausefwd` duration saturates at 1440 minutes (24h) to prevent
  accidental weeks-long pauses.
- The existing `/setforward on` path should clear `s_fwdPauseUntilMs` so
  manually re-enabling forwarding also cancels any pending auto-resume.
