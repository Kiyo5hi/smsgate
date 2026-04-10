---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0098: /mute and /unmute commands for proactive alerts

## Motivation

During maintenance windows the operator may want to silence proactive alerts
(stuck-queue, low-CSQ, registration-lost) without disabling the bridge. A
`/mute [minutes]` command snoozes all proactive alerts for the given duration
(default 60 minutes); `/unmute` cancels the snooze early.

## Design

- Add `void setMuteFn(std::function<void(uint32_t minutes)> fn)` to
  TelegramPoller. When `/mute` is received, calls `fn(minutes)`.
- Add `void setUnmuteFn(std::function<void()> fn)` to TelegramPoller.
  When `/unmute` is received, calls `fn()`.
- In `main.cpp`, add `s_alertsMutedUntilMs` state variable.
- Wire `setMuteFn` to a lambda that sets
  `s_alertsMutedUntilMs = millis() + minutes * 60000`.
- Wire `setUnmuteFn` to a lambda that sets `s_alertsMutedUntilMs = 0`.
- Guard stuck-queue, low-CSQ, and reg-lost alert send calls with
  `if (millis() >= s_alertsMutedUntilMs)`.

## File changes

**`src/telegram_poller.h`** — add setMuteFn / setUnmuteFn  
**`src/telegram_poller.cpp`** — add /mute, /unmute handlers; add to /help  
**`src/main.cpp`** — add s_alertsMutedUntilMs, wire setters, guard alerts  
**`src/telegram.cpp`** — register commands  
**`test/test_native/test_telegram_poller.cpp`** — test /mute and /unmute
