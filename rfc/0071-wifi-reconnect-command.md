---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0071: /wifi force reconnect command

## Motivation

After a prolonged WiFi outage the ESP32's WiFi stack sometimes refuses to
reconnect automatically. The 30-second transport-check loop in `loop()`
(added in earlier RFCs) already attempts reconnects, but the user has no
way to trigger one on demand — they either wait or physically power-cycle
the board.

A `/wifi` bot command gives the operator a one-tap escape hatch that
disconnects the adapter and calls `connectToWiFi()` immediately, without
requiring a full reboot.

## Plan

**`src/telegram_poller.h`**:
- Add `void setWifiReconnectFn(std::function<void()> fn)` setter.
- Add `std::function<void()> wifiReconnectFn_;` private member.

**`src/telegram_poller.cpp`**:
- Add `/wifi` handler: send "🔄 WiFi reconnect initiated." then call
  `wifiReconnectFn_()` if set; fallback "(WiFi reconnect not configured)".
- Add `/wifi — Force WiFi reconnect` to `/help` output.

**`src/main.cpp`**:
- Add `static bool s_pendingWifiReconnect = false;` near `s_pendingRestart`.
- In loop(), after the `s_pendingRestart` block: when the flag is set,
  clear it, call `WiFi.disconnect(true)`, delay 500 ms, call
  `connectToWiFi()`, then `setupTelegramClient(realBot)`. On success send
  "🟢 WiFi reconnected."; on TLS failure send "⚠️ WiFi up but Telegram
  TLS failed.".
- Wire: `telegramPoller->setWifiReconnectFn([]() { s_pendingWifiReconnect = true; });`

**`src/telegram.cpp`**:
- Register `/wifi` command with description "Force WiFi reconnect".
- Update the Serial log string to include `/wifi`.

## Notes for handover

Changed: `src/telegram_poller.{h,cpp}`, `src/main.cpp`, `src/telegram.cpp`,
`rfc/0071-wifi-reconnect-command.md`.

No new tests: the callback-injection pattern is identical to `/ntp`
(`setNtpSyncFn`) and is already exercised by the existing poller test suite.
