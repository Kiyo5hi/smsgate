---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0167: /settings command — runtime configuration snapshot

## Motivation

There is no single command to inspect all currently active runtime settings.
Users must run /setforward, /setblockmode, /setmaxparts, /setcallnotify,
/setcalldedup, /setunknowndeadline, /setpollinterval, /setinterval, etc. to
learn what values are currently in effect. `/settings` provides a one-shot
snapshot of all runtime-configurable parameters.

## Plan

Add `setSettingsFn(std::function<String()>)` to TelegramPoller.
Add `/settings` handler that calls `settingsFn_()` and replies with the
formatted snapshot. Wire in main.cpp with a lambda that captures all
relevant objects and returns a formatted multi-line string.
