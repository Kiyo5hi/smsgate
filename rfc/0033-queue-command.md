---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0033: `/queue` command — inspect outbound SMS queue

## Motivation

If the modem has trouble sending an SMS (bad signal, busy), the entry sits in
`SmsSender`'s retry queue silently. The user has no way to know what is pending
without reading the serial log. `/queue` provides a one-shot snapshot so the user
can see what is waiting and how many attempts have been made.

## Plan

### `sms_sender.h` / `sms_sender.cpp`

Add a read-only snapshot accessor:

```cpp
struct QueueSnapshot { String phone; String bodyPreview; int attempts; };
std::vector<QueueSnapshot> getQueueSnapshot() const;
```

Returns one entry per occupied queue slot, with `bodyPreview` capped at 20 chars.

### `telegram_poller.cpp`

Add `/queue` handler between `/send` and the fallback help:

```
📤 Queue empty

— or —

📤 Queue: 2 pending
1. +8613800138000 "Hello world this is…" (attempt 2/5)
2. +1234567890 "Test" (attempt 1/5)
```

### `telegram.cpp`

Add `/queue` to registered bot commands (DynamicJsonDocument 768 → 896).

### Help text

Updated to: `… /send <num> <msg>, /queue`

## Notes for handover

- `src/sms_sender.h` / `sms_sender.cpp` — `QueueSnapshot` struct + `getQueueSnapshot()`
- `src/telegram_poller.cpp` — `/queue` dispatch block
- `src/telegram.cpp` — bot command registration
- 4 new tests: 2 in `test_sms_sender.cpp` (empty + captures entry),
  2 in `test_telegram_poller.cpp` (empty queue reply, non-empty queue shows phone)
