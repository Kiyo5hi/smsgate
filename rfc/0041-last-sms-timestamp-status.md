---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0041: Last SMS received timestamp in `/status`

## Motivation

`/status` shows lifetime forwarded count but not *when* the last SMS arrived.
After a suspected connectivity gap or modem reset the user wants to know "was
the last SMS before or after the incident?" without trawling the serial log.

## Plan

**`src/sms_handler.h`** — add optional forward callback:
```cpp
void setOnForwarded(std::function<void()> cb) { onForwarded_ = std::move(cb); }
std::function<void()> onForwarded_;
```

**`src/sms_handler.cpp`** — fire the callback at both success points
(`forwardSingle` and `insertFragmentAndMaybePost`), immediately after
`smsForwarded_++`:
```cpp
if (onForwarded_) onForwarded_();
```

**`src/main.cpp`**
- Add `static time_t s_lastSmsTimestamp = 0;` alongside `s_bootTimestamp`.
- After `smsHandler.setDebugLog(...)`:
  ```cpp
  smsHandler.setOnForwarded([]() { s_lastSmsTimestamp = time(nullptr); });
  ```
- In statusFn SMS section, after the Forwarded line:
  ```cpp
  if (s_lastSmsTimestamp > 0) {
      time_t lt = s_lastSmsTimestamp + TIMEZONE_OFFSET_SEC;
      strftime(lastSmsBuf, sizeof(lastSmsBuf), "%Y-%m-%d %H:%M", gmtime(&lt));
      msg += "  Last rcvd: " + lastSmsBuf + " " + tzLabel + "\n";
  }
  ```

Result: `Last rcvd: 2026-04-10 14:32 UTC+8`

## Notes for handover

`s_lastSmsTimestamp` is RAM-only (resets to 0 on reboot). Only
`src/sms_handler.{h,cpp}` and `src/main.cpp` changed. No test changes
needed — the callback mechanism is already exercised by the existing
`smsForwarded_` path.
