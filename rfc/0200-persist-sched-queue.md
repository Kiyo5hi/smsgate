---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0200: Persist scheduled SMS queue to NVS

## Motivation

RFC-0188 introduced a 5-slot in-memory scheduled SMS queue. The queue is
lost on reboot ("Note: lost on reboot." in the `/schedulesend` reply).
Now that the partition change (RFC-0199) freed ~1.96MB of flash, we can
afford to serialize the queue.

## Design

### Serialization format

NVS key `sched_queue` (blob), version byte 0x01.

Each slot is fixed 168 bytes:
```
uint8_t  occupied;          // 1 = occupied, 0 = free
uint8_t  reserved[3];       // padding
uint32_t sendAtUnix;        // Unix timestamp (UTC) when to fire
char     phone[32];         // destination phone number (null-terminated)
char     body[128];         // message body (null-terminated, truncated if needed)
```

Total: 5 × 168 = 840 bytes + 1 version byte = 841 bytes per blob.

### Conversion: millis-based → Unix-based

When scheduling (`/schedulesend`):
- `sendAtMs` (millis-based) remains the in-memory representation.
- Convert to Unix: `sendAtUnix = time(nullptr) + (sendAtMs - millis()) / 1000`.
- Persist blob with `sendAtUnix`.

When loading at boot:
- Convert back: `sendAtMs = millis() + (sendAtUnix - time(nullptr)) * 1000`.
- If `sendAtUnix <= time(nullptr)` (already past): fire immediately (set
  `sendAtMs = 0` so drain triggers), or skip if the overdue threshold
  is > 2h (stale).

### NVS persistence calls

- After any `/schedulesend`, `/cancelsched`, `/clearschedule`, `/scheddelay`,
  `/schedrename`, `/sendnow` mutation: save the blob.
- At boot (after NTP sync, since we need time(nullptr)): load the blob.

### TelegramPoller changes

Add:
- `setPersistSchedFn(std::function<void()> fn)` — called after any queue mutation
  to trigger a save. The lambda in main.cpp serializes the queue and calls
  `realPersist.saveBlob`.
- `setLoadSchedFn(std::function<void()> fn)` — called once in setup() after NTP
  sync to load the queue from NVS.

Wait — the queue is internal to TelegramPoller. To serialize it, the poller
needs to expose a snapshot. Add:

```cpp
// Returns a snapshot of the scheduled queue for NVS serialization.
std::vector<ScheduledSms> getSchedSnapshot() const;
// Replace queue contents from a loaded snapshot.
void setSchedSnapshot(const std::vector<ScheduledSms> &snap);
```

The snapshot uses `sendAtMs` (millis-based). The conversion to/from Unix
happens in the `persistSchedFn_` / `loadSchedFn_` lambdas in main.cpp.

### main.cpp wiring

```cpp
// Save
auto persistSched = [&]() {
    // serialize 5 slots from telegramPoller->getSchedSnapshot()
    // write to realPersist
};
telegramPoller->setPersistSchedFn(persistSched);

// Load (call after NTP sync in setup())
auto loadSched = [&]() {
    // read blob, deserialize, convert to millis, call setSchedSnapshot
};
```

## Notes for handover

- The load must happen AFTER NTP sync (so `time(nullptr)` is valid).
  In main.cpp, call `telegramPoller->loadScheduledSms()` (or equivalent)
  right after `syncTime()` returns.
- Slots past their `sendAtUnix` by < 2h are re-queued to fire immediately.
  Slots past by >= 2h are dropped as stale.
- `body` is truncated at 127 chars if the user sent a long message;
  the send will proceed with the truncated body.
