---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0185: Persist callNotifyEnabled and pollIntervalMs to NVS

## Motivation

Completing the NVS persistence sweep (RFC-0182, RFC-0183). Two runtime
settings remain volatile:

- `/setcallnotify off` — operator silences call alerts, then a watchdog
  reset re-enables them silently.
- `/setpollinterval <s>` — operator tunes polling rate to reduce data
  cost, then a reboot resets it to the default 3s.

## Plan

### NVS keys

| Key            | Type     | Default   | Notes                           |
|----------------|----------|-----------|---------------------------------|
| `call_notify`  | uint8_t  | 1         | 1=ON, 0=OFF                     |
| `poll_ms`      | uint32_t | 3000      | Milliseconds; validated on load |

### Load at boot

```cpp
// RFC-0185: Restore call notify flag and poll interval.
{
    uint8_t v = 1;
    if (realPersist.loadBlob("call_notify", &v, sizeof(v)) == sizeof(v))
        callHandler.setCallNotifyEnabled(v != 0);
}
{
    uint32_t v = 0;
    if (realPersist.loadBlob("poll_ms", &v, sizeof(v)) == sizeof(v)
        && v >= 1000 && v <= 30000)
        telegramPoller->setPollIntervalMs(v);
}
```

Note: `callHandler` setup must complete before the NVS load, and
`telegramPoller` pointer must be valid (both are, as NVS is loaded
inside the NVS-success block which runs after both objects are created).

### Save on mutation

In `setCallNotifyFn` lambda:
```cpp
callHandler.setCallNotifyEnabled(enable);
uint8_t v = enable ? 1 : 0;
realPersist.saveBlob("call_notify", &v, sizeof(v));
```

In `setPollIntervalFn` lambda (inside `/setpollinterval` handler):
Actually `setPollIntervalMs` is called directly on the poller inside
`TelegramPoller::processUpdate`. We need a side-channel. The cleanest
fix: add a `setPollIntervalFn` setter on `TelegramPoller` that is called
in addition to the internal `pollIntervalMs_` update, so `main.cpp` can
attach the NVS save.

Or simpler: `setPollIntervalMs` is called inside `processUpdate` directly.
Add an optional `onPollIntervalChanged_` callback to `TelegramPoller`
triggered whenever `pollIntervalMs_` is mutated.

Simplest: just persist after mutation via a dedicated setter:
```cpp
telegramPoller->setOnPollIntervalChangedFn([](uint32_t ms) {
    realPersist.saveBlob("poll_ms", &ms, sizeof(ms));
});
```

## Notes for handover

- poll_ms validation on load: reject values outside [1000, 30000] to
  avoid persisted garbage from future format changes.
- The `callHandler` pointer in main.cpp is a local reference; the load
  block must run after `callHandler` construction.
