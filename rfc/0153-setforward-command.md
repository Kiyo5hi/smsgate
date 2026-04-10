---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0153: /setforward command — toggle SMS forwarding

## Motivation

During maintenance (testing, SIM swap) it's useful to pause SMS
forwarding without rebooting. Incoming SMS are received and stay in
SIM slots; use /sweepsim to process them when ready.

## Plan

- Add `forwardingEnabled_` bool (default true) to SmsHandler.
- `handleSmsIndex()` returns early (without deleting from SIM) when
  forwarding is disabled, so messages accumulate safely.
- Add `setForwardingEnabledFn(std::function<void(bool)>)` setter to
  TelegramPoller.
- Command: `/setforward on` or `/setforward off`.
- In `main.cpp` wire: lambda calls `smsHandler.setForwardingEnabled(b)`.
