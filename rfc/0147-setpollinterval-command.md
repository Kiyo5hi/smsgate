---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0147: /setpollinterval command — change Telegram poll interval

## Motivation

`TelegramPoller::kPollIntervalMs = 3000ms` is compile-time. In quiet
environments a longer interval saves data; during active use a shorter
interval reduces reply latency. Runtime control avoids reflashing.

## Plan

- Change `kPollIntervalMs` in `TelegramPoller` from `static constexpr`
  to a mutable member `pollIntervalMs_`, defaulting to the constant.
- Add `setPollIntervalFn(std::function<void(uint32_t)>)` setter. The fn
  called by the command is also wired back to set `pollIntervalMs_` so
  TelegramPoller uses the new interval on the next tick.
- Simpler: add a direct setter `setPollIntervalMs(uint32_t ms)` to
  TelegramPoller itself (no lambda indirection needed).
- Command: `/setpollinterval <seconds>` (range: 1–300 seconds).
  0 is not allowed — polling must continue for commands to work.
