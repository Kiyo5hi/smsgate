---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0139: /flushsim command — delete all SMS from SIM

## Motivation

After extended operation the SIM can fill up (typically 20–30 slots),
causing new SMS to be rejected by the modem. A single Telegram command
to clear all stored messages avoids attaching a serial monitor.

## Plan

- Add `setFlushSimFn(std::function<int()>)` setter to `TelegramPoller`.
  The fn deletes all SMS and returns the count (-1 if count unknown).
- Command `/flushsim yes` (the "yes" argument is required to prevent
  accidental deletion). Without "yes" → usage error explaining the syntax.
- In `main.cpp` wire via `AT+CMGDA="DEL ALL"` (A76xx extension) then
  re-issue `sweepExistingSms` to drain any that survived.

## Notes for handover

AT+CMGDA is an A76xx/SIM7xxx extension, not in the core GSM standard.
The fn should attempt CMGDA first; on failure fall back to iterating
CMGL and deleting individually. Return value is advisory (for the reply
message); failures are logged but don't prevent further deletes.
