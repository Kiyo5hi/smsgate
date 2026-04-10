---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0034: Outbound queue depth in `/status`

## Motivation

`SmsSender::queueSize()` exists but was not surfaced in `/status`. A user sending an SMS
that gets stuck retrying (bad signal, modem issue) had no way to tell whether their message
was still queued or had been silently dropped — without reading the serial log.

## Plan

In `main.cpp` `statusFn`, after "Concat in-flight" in the SMS section:

```cpp
msg += "  Outbound queue: ";
msg += String(smsSender.queueSize()); msg += "/";
msg += String(SmsSender::kQueueSize); msg += "\n";
```

Result: `Outbound queue: 1/8` when one SMS is pending retry, `0/8` when idle.

## Notes for handover

Only `src/main.cpp` changed. No test changes needed (statusFn is not unit-tested).
