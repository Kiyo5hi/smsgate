---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0160: /setmaxparts command — runtime-configurable outbound SMS concat parts

## Motivation

The maximum number of SMS parts for outbound concat messages is hardcoded at 10
in `sms_sender.cpp`. Operators on expensive per-part SMS plans may want to cap
outbound messages at e.g. 3 parts to control costs.

## Plan

Add `setMaxParts(int n)` / `maxParts()` to `SmsSender`. Pass `maxParts_` to
`buildSmsSubmitPduMulti` instead of the hardcoded `10`. Add
`setMaxPartsFn(std::function<void(int)>)` to TelegramPoller and handle
`/setmaxparts <N>` (validates 1–10, replies with char-limit summary).
Wire in main.cpp: `smsSender.setMaxParts(n)`.
