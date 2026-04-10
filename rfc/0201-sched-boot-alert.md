---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0201: Telegram alert when scheduled SMS are restored at boot

## Motivation

RFC-0200 persists the scheduled SMS queue to NVS. When the device boots
and re-loads N scheduled slots, the user has no way to know (other than
checking `/schedqueue`) that their messages survived the reboot. A brief
Telegram notification closes this gap.

## Design

After the RFC-0200 load block in `main.cpp`, if `loaded > 0`, send a
Telegram message via `realBot.sendMessage(...)` in the boot banner
section. The message is appended to the existing boot banner string
(which is sent right after `telegramPoller->begin()`):

```
⏰ Restored N scheduled SMS from NVS. Use /schedqueue to review.
```

The simplest approach: pass `loaded` count out of the load block and
include the alert in the boot banner string that's already assembled
and sent in `setup()`.

## Notes for handover

- The boot banner is assembled in the big `switch (s_resetReason)` block
  and sent via `realBot.sendMessage(bootMsg)`. Append to `bootMsg` if
  `schedLoaded > 0`.
- If 0 slots were loaded, no alert is sent (no noise on normal boots).
