---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0028: Watchdog kick inside `doSendMessage`

## Motivation

`doSendMessage()` in `telegram.cpp` blocks for up to 8 s on TLS response
drain. Multiple back-to-back command replies from `processUpdate()` can
accumulate to 16+ seconds without a WDT kick, potentially triggering the
120s hardware watchdog if any exchange hangs. RFC-0015 already guards the
heartbeat path and SmsSender; this fills the remaining gap.

## Plan

Add `#include <esp_task_wdt.h>` (ESP_PLATFORM-guarded) and one
`esp_task_wdt_reset()` call at the entry of `doSendMessage()`, after
the chatId/transport guards and before URL construction.

## Notes for handover

Only `src/telegram.cpp` changed. No test changes needed (file excluded
from native build by `build_src_filter`).
