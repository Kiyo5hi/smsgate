---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0035: Outbound SMS failures logged to debug log

## Motivation

When `SmsSender` drops an SMS (queue full or final failure after all retries), it sends
an error reply via Telegram and logs to serial. But if Telegram was unreachable at the
time, the notification is lost and there is no post-hoc record. `/debug` already exposes
the SMS receive log; adding outbound failure entries gives the user the same visibility
for sends.

## Plan

Add `setDebugLog(SmsDebugLog *)` setter to `SmsSender` (same pattern as
`setDeliveryReportMap`). Wire it in `main.cpp` alongside the other debug log wiring.

On queue-full rejection (in `enqueue`):
```cpp
SmsDebugLog::Entry le;
le.timestampMs = millis();   // 0 in native tests
le.sender    = phone;        // repurposed: recipient
le.bodyChars = body.length();
le.outcome   = "out:queue_full";
le.pduPrefix = body.substring(0, 40);  // body preview
debugLog_->push(le);
```

On final failure after kMaxAttempts (in `drainQueue`):
```cpp
le.timestampMs = nowMs;
le.outcome     = String("out:fail: ") + lastError_;
```

The `out:` prefix distinguishes outbound entries in the `/debug` dump from inbound entries.
`pduPrefix` stores the first 40 chars of the body as a preview (label "PDU:" is repurposed).

## Notes for handover

- `src/sms_sender.h` — `SmsDebugLog` forward decl, `setDebugLog()`, `debugLog_` member
- `src/sms_sender.cpp` — `#include "sms_debug_log.h"`, logging in `enqueue` and `drainQueue`
- `src/main.cpp` — `smsSender.setDebugLog(&smsDebugLog)` in the NVS init block
- 2 new tests in `test_sms_sender.cpp`: queue_full and final_fail log to debug log
