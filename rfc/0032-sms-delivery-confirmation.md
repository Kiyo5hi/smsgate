---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0032: SMS delivery confirmation via `onSuccess` callback

## Motivation

`SmsSender::enqueue` accepted an `onFinalFailure` callback but had no `onSuccess`
counterpart. The user received "✅ Queued reply to +xxx" immediately on enqueue but
got no feedback when the SMS was actually transmitted. If the first attempt fails
and the bridge retries silently, there is no signal that the SMS ever went through.
A delivery confirmation closes this gap.

## Plan

### `sms_sender.h` / `sms_sender.cpp`

Add `onSuccess` field to `OutboundEntry` and a matching optional parameter to
`enqueue`:

```cpp
bool enqueue(const String &phone, const String &body,
             std::function<void()> onFinalFailure = nullptr,
             std::function<void()> onSuccess = nullptr);
```

In `drainQueue`, on successful send copy and call `onSuccess` before clearing
the slot (same pattern as `onFinalFailure`).

### `telegram_poller.cpp`

Both paths pass a delivery notification lambda as `onSuccess`:

```cpp
// reply path
smsSender_.enqueue(phone, u.text, onFailureLambda,
    [this, capturedPhone, requesterChatId]() {
        bot_.sendMessageTo(requesterChatId,
            String("\xF0\x9F\x93\xA8 Sent to ") + capturedPhone); // 📨
    });

// /send path — identical pattern
```

The user sees two messages per outbound SMS:
1. "✅ Queued reply to +xxx" — immediate (enqueue accepted)
2. "📨 Sent to +xxx" — async (SMS transmitted successfully)

If the SMS fails after all retries, they get "❌ SMS to +xxx failed after retries"
instead of (2).

## Notes for handover

- `src/sms_sender.h` and `src/sms_sender.cpp` — `onSuccess` field + param
- `src/telegram_poller.cpp` — onSuccess lambdas for both /send and reply paths
- Two new tests in `test_sms_sender.cpp`: onSuccess fires on success, does not
  fire on failure
- Two new tests in `test_telegram_poller.cpp`: delivery notification appears after
  drainQueue succeeds for both reply and /send paths
