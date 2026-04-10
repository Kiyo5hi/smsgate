---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0139: /ntpresync command — trigger NTP resync from Telegram

## Motivation

After extended uptime or network glitches the ESP32's clock can drift.
Currently the only way to force an NTP resync is to reboot. This command
lets the operator trigger a resync without interrupting the bridge.

## Plan

Add a `setNtpResyncFn(std::function<String()>)` setter to `TelegramPoller`.
The function performs the resync and returns a status string (new time or
error message).

Command: `/ntpresync` → calls `ntpResyncFn_()`, replies with the result.

## Notes for handover

`configTime()` + `getLocalTime()` is the ESP32 NTP path.
`main.cpp` already has `syncNtp()` logic in `setup()`; extract it into a
lambda that can be called from the loop.
