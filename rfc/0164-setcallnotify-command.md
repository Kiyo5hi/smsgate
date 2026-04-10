---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0164: /setcallnotify command — toggle call Telegram notifications

## Motivation

In environments with frequent spam calls, the Telegram notification flood
is annoying. `/setcallnotify off` mutes call notifications while keeping
auto-reject behavior. `/setcallnotify on` restores notifications.

## Plan

Add `setCallNotifyEnabled(bool)` / `callNotifyEnabled()` to `CallHandler`.
Wrap the `bot_.sendMessage(...)` call in `commitEvent()` with
`if (callNotifyEnabled_)`. Add `setCallNotifyFn(std::function<void(bool)>)`
to TelegramPoller and handle `/setcallnotify on|off`. Wire in main.cpp.
