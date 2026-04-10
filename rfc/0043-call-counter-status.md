---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0043: Call counter in `/status`

## Motivation

`/status` shows SMS forwarded/failed counts but no call statistics. After a
period away, the user may want to know how many calls were auto-rejected since
the last reboot without trawling the Telegram history.

## Plan

**`src/call_handler.h`** — add counter and getter:
```cpp
int callsReceived() const { return callsReceived_; }
int callsReceived_ = 0;
```

**`src/call_handler.cpp`** — increment in `commitRinging()` before the
Telegram notify:
```cpp
callsReceived_++;
```

**`src/main.cpp`** — add to statusFn SMS section:
```cpp
msg += "  Calls rcvd: " + String(callHandler.callsReceived()) + "\n";
```

## Notes for handover

RAM-only counter — resets to 0 on reboot. Only
`src/call_handler.{h,cpp}` and `src/main.cpp` changed. No test changes
needed.
