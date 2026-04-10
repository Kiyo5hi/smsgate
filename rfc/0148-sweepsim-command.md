---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0148: /sweepsim command — manually trigger SIM sweep

## Motivation

The SIM sweep (`smsHandler.sweepExistingSms()`) runs automatically at
startup. After a transient failure leaves SMS stuck in the SIM, a
manual trigger avoids having to reboot the device.

## Plan

Add `setSweepFn(std::function<int()>)` setter to `TelegramPoller`.
The fn calls `smsHandler.sweepExistingSms()` and returns the count of
SMS dispatched. Reply: "✅ Swept N SMS." (0 → "(no SMS found)").
