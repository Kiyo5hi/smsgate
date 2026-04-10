---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0208: Track and display last successful Telegram contact time

## Motivation

If the WiFi drops silently (no DHCP, TLS errors) the device may appear
alive (heartbeat still fires from the modem path) while actually never
reaching api.telegram.org. `/status` has "Last NTP sync" but not "Last
successful Telegram API response". The operator has no early warning.

## Design

Add `setOnPollSuccessFn(std::function<void()> fn)` to TelegramPoller.
Called after each successful `pollUpdates()` (HTTP 200 + valid JSON
envelope), regardless of whether any updates were present.

In main.cpp:
- `static time_t s_lastTelegramOkTime = 0;`
- Wire: `telegramPoller->setOnPollSuccessFn([]() { s_lastTelegramOkTime = time(nullptr); });`
- In `/status` lambda, after "Last NTP:", add:
  ```
  Last TG: <timestamp> <tz>   (or "(never)" if not yet polled)
  ```

## Notes for handover

- The callback is placed in `tick()` right after the `IBotClient::pollUpdates()`
  call returns true (transport success). A 200-OK with `"ok":false` from
  Telegram still counts as a successful TCP/TLS contact — the callback fires
  regardless of the inner `ok` field.
- `s_lastTelegramOkTime` is not persisted to NVS — it resets on each boot.
  Showing "(never)" at boot until the first successful poll is correct.
