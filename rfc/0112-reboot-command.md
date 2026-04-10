---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0112 — /reboot command

## Motivation

The device can get into a soft-stuck state (modem unresponsive, TLS
session wedged, WiFi stale) that the watchdog doesn't catch because the
main loop is still running. Currently the only recovery is a physical
power cycle. Adding a `/reboot` Telegram command lets an operator trigger
a clean `ESP.restart()` remotely without touching the hardware.

## Plan

1. Add a `setRebootFn(std::function<void()> fn)` setter to
   `TelegramPoller` (alongside the existing `setStatusFn` etc.).

2. In `doHandleMessage`, add a `/reboot` case:
   - Reply `"Rebooting..."` via `sendInfoReply`.
   - Call `rebootFn_()` if set; otherwise reply "not configured".

3. In `main.cpp`, wire:
   ```cpp
   poller.setRebootFn([]() { ESP.restart(); });
   ```

4. Register `/reboot` in `setMyCommands` in `telegram.cpp`.

5. Tests:
   - Reboot fn called and "Rebooting" reply sent.
   - Fn not set → "not configured" reply.

## Notes for handover

The reply is sent *before* calling `rebootFn_()` so the user sees
confirmation before the device goes dark. The bot client's keep-alive
socket will be closed by the restart, so there is no teardown needed.
