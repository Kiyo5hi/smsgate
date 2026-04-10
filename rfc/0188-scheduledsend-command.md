---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0188: `/scheduledsend` — Delayed SMS delivery

## Motivation

Sometimes the user wants to send an SMS at a specific time (e.g. a
reminder, a message to send when the recipient wakes up). Currently
the only option is to send immediately via `/send`. A scheduled
queue lets the user queue a future delivery from Telegram.

## Plan

### Slots

5-slot fixed array in `TelegramPoller`. Each slot:
```cpp
struct ScheduledSms {
    uint32_t sendAtMs;  // millis() target; 0 = slot free
    String   phone;
    String   body;
};
```

### Commands

- `/schedulesend <delay_min> <phone> <body>` — Schedule an SMS.
  `delay_min`: 1–1440 (up to 24 hours).
  Replies: "⏰ SMS to <phone> scheduled in <N> min (slot <i>/5)."
  Error if all 5 slots are occupied.

- `/schedqueue` — List all pending scheduled SMS.
  Shows slot index, phone, eta (minutes remaining), body preview.

- `/cancelsched <N>` — Cancel slot N (1-based). Clears the slot.

### tick() integration

In `tick()`, before the HTTP poll, iterate all occupied slots and
call `smsSender_.enqueue(phone, body)` for slots where
`nowMs >= sendAtMs`. Clear the slot on success. On failure
(SmsSender::enqueue returns false — queue full) leave the slot
for next tick.

## Notes for handover

- Slots are volatile (RAM only) — a reboot discards all pending
  scheduled SMS. The user is informed of this via the reply text:
  "Note: scheduled SMS are lost on reboot."
- No NVS persistence for simplicity (5 slots × RAM).
- The 1-minute minimum prevents scheduling "now + 0ms" which would
  be confusing (use /send for immediate dispatch).
