---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0086: Log outbound SMS success to SmsDebugLog

## Motivation

RFC-0035 added `SmsDebugLog` entries for outbound failures. Successful
sends are only reported to Serial. When debugging why an expected reply
never arrived, the operator has no record of whether the outbound SMS
was actually sent. Adding a success log entry (outcome "out:sent")
gives a complete audit trail visible via `/debug`.

## Plan

**`src/sms_sender.cpp`**:
- In the `ok` branch of `drainQueue`, after `e.occupied = false`:
  ```cpp
  if (debugLog_) {
      SmsDebugLog::Entry le;
      le.timestampMs = nowMs;
      le.sender      = e.phone;   // repurposed: recipient for outbound
      le.bodyChars   = (uint16_t)(e.body.length() > 65535u ? 65535u : e.body.length());
      le.outcome     = String("out:sent");
      le.pduPrefix   = e.body.substring(0, 40);
      debugLog_->push(le);
  }
  ```

## Notes for handover

Changed: `src/sms_sender.cpp`, `rfc/0086-outbound-sms-success-log.md`.
No new tests — the debug log push path is straightforward.
