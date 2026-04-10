---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0024: Debug log entries for blocked SMS

## Motivation

When a sender is added to the block list via `/block`, their subsequent SMS are
silently deleted from the SIM. The only way to verify the block is working is to
watch the serial monitor. The Telegram `/debug` command shows no trace of blocked
messages — the `SmsDebugLog` capture block in `handleSmsIndex` is placed *after*
the block-list early-return, so blocked SMS never reach it.

Adding a `SmsDebugLog::Entry` with `outcome = "blocked"` for every blocked
message gives `/debug` the observability it lacks without requiring a serial
connection.

## Plan

In `src/sms_handler.cpp`, inside the block-list check branch, push a debug log
entry before the `AT+CMGD` deletion:

```cpp
if (debugLog_)
{
    SmsDebugLog::Entry e;
    e.timestampMs = clock_ ? clock_() : 0;
    e.sender      = pdu.sender;
    e.bodyChars   = (uint16_t)pdu.content.length();
    e.isConcat    = pdu.isConcatenated;
    e.concatRef   = pdu.concatRefNumber;
    e.concatTotal = pdu.concatTotalParts;
    e.concatPart  = pdu.concatPartNumber;
    e.outcome     = String("blocked");
    debugLog_->push(e);
}
```

No schema changes. The `outcome` field accepts free-form strings; `"blocked"`
fits in the existing `PersistEntry::body[101]` slot.

## Notes for handover

- Only `src/sms_handler.cpp` changes.
- No new tests required: existing blocked-SMS test fixtures already call
  `smsHandler.setDebugLog(&log)`; the blocked-entry count can be asserted
  there.
