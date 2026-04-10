---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0102 — Boot uptime in /status

## Motivation

`/status` shows signal quality, queue depth, and SMS counters but not how
long the device has been running. Uptime is the single most useful indicator
of device health between heartbeats: a very short uptime reveals a crash loop;
a very long uptime gives confidence the device has been stable.

## Plan

1. Record `s_bootMs = millis()` at the end of `setup()` in `main.cpp`,
   just after the boot-complete Telegram notification.

2. In the `/status` lambda, compute uptime from `millis() - s_bootMs` and
   format it as a human-readable string (e.g. `"3d 14h 22m"`, `"4h 7m"`,
   `"43m"`, `"< 1m"`).

3. Append the uptime string to the System section of the status message
   (next to the build timestamp).

No new interface, setter, or test double required — the status lambda captures
`s_bootMs` from the `main.cpp` scope and `millis()` is a free function already
used in the firmware.

## Notes for handover

The status string is assembled inside the `statusFn_` lambda passed to
`TelegramPoller`, so no changes to `telegram_poller.{h,cpp}` are needed.
