---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0104 — /sendall delivery summary

## Motivation

`/sendall <msg>` broadcasts to all aliases but currently only confirms
"Queued to N recipients". The user has no way to know how many of the N
SMS actually made it through the modem — especially if the queue was half
full or the modem was in a bad state.

## Plan

1. In the `/sendall` handler in `TelegramPoller::processUpdate`, allocate a
   `std::shared_ptr<BatchState>` capturing `{ total, succeeded=0, failed=0,
   chatId, reported=false }`.

2. Pass the shared_ptr into every enqueue `onFailure` / `onSuccess` lambda.
   When `succeeded + failed == total` and `!reported`, set `reported=true` and
   send the summary message `"📊 Delivered N/M"` (or with failure detail if
   any failed: `"📊 N/M delivered, F failed"`).

3. The shared_ptr lives as long as any callback holds a copy — no raw pointer
   escapes, no lifetime issue.

4. `#include <memory>` is already transitively available in telegram_poller.cpp
   via the C++ stdlib. Guard with `std::make_shared`.

5. Tests: wire two aliases, let one succeed and one fail; assert the summary
   message shows "1/2" and mentions the failure.

## Notes for handover

The lambdas are stored inside `SmsSender::OutboundEntry`. The
`SmsSender` destructor fires callbacks with an abandoned state — that's
fine because the `shared_ptr` outlives the `SmsSender` only if a callback
is still pending when the `SmsSender` is destroyed; in practice both objects
have process lifetime. If `SmsSender` is destroyed before the callbacks
fire (e.g. a reboot), the `reported` flag prevents a double-send and the
lambda simply decrements the ref count harmlessly.
