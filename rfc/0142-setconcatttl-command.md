---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0142: /setconcatttl command — change concat fragment TTL at runtime

## Motivation

`SmsHandler::CONCAT_TTL_MS = 24h` is compile-time. In high-traffic
environments with known partial-message patterns it can be useful to
shorten or extend the TTL without reflashing.

## Plan

- Change `CONCAT_TTL_MS` in `SmsHandler` from `static constexpr` to a
  runtime field `concatTtlMs_` defaulting to the constant.
- Add `void setConcatTtlMs(unsigned long ms)` setter.
- Add `setContatTtlFn(std::function<void(uint32_t)>)` setter to `TelegramPoller`.
- Command: `/setconcatttl <seconds>` (range: 60–604800, i.e. 1min–7days).
- In `main.cpp` wire: lambda calls `smsHandler.setConcatTtlMs(seconds * 1000UL)`.
  No NVS persist (reboot resets to 24h, which is the safe default).

## Notes for handover

The TTL check lives in `SmsHandler::checkConcatTtl()`. After changing the
field, the next TTL sweep picks up the new value automatically.
