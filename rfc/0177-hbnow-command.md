---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0177: /hbnow command — trigger immediate heartbeat

## Motivation

After changing the heartbeat interval or message format, operators want to
verify the output immediately without waiting up to 6 hours for the next
scheduled send. `/hbnow` forces the next `loop()` tick to fire the heartbeat.

## Current state

The heartbeat fires automatically in `loop()` based on `lastHeartbeatMs`.
No command exists to trigger it on demand.

## Plan

1. **`TelegramPoller`** — add `setHeartbeatNowFn(std::function<void()> fn)`.
   `/hbnow` calls this fn and replies "✅ Heartbeat triggered."

2. **`main.cpp`** — wire:
   ```cpp
   telegramPoller->setHeartbeatNowFn([]() {
       lastHeartbeatMs = millis() - s_heartbeatIntervalSec * 1000UL - 1;
   });
   ```
   This makes the heartbeat fire on the very next `loop()` iteration.
   If `s_heartbeatIntervalSec == 0` (heartbeat disabled), the fn does nothing
   and the reply says "Heartbeat is disabled — enable with /setinterval first."

3. **`telegram.cpp`** — add `hbnow` to `setMyCommands`.

4. **Tests** — one poller test: `/hbnow` calls the fn and replies success.

## Notes for handover

- `lastHeartbeatMs` is a `static` local to the `[HEARTBEAT_INTERVAL_SEC != 0]`
  conditional block. It needs to be promoted to a file-scope static with
  `#if HEARTBEAT_INTERVAL_SEC != 0` guard removed so the lambda can capture
  it unconditionally, OR the lambda captures a pointer.
- Simplest approach: make `lastHeartbeatMs` unconditionally file-scope and
  guard usage with `if (s_heartbeatIntervalSec > 0)` at the call site.
