---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0072: /heap diagnostic command

## Motivation

`/status` shows free heap in its summary but requires parsing a long
multi-line reply. Operators debugging memory-related instability want
a quick, machine-readable snapshot: free heap, minimum ever recorded
(low-water mark), and largest contiguous block. This also gives a
convenient "is the device still alive?" check that returns a number
rather than a long string.

## Plan

**`src/telegram_poller.h`**:
- Add `void setHeapFn(std::function<String()> fn)` setter.
- Add `std::function<String()> heapFn_;` private member.

**`src/telegram_poller.cpp`**:
- Add `/heap` handler: call `heapFn_()` if set; fallback "(heap info
  not configured)".
- Add `/heap — Show free/min/max-block heap` to `/help`.

**`src/main.cpp`**:
- Wire: `telegramPoller->setHeapFn([]() -> String { ... });`
  Returns e.g. `"Free: 42312 B  Min: 38900 B  Max block: 40960 B"`.
  Uses `ESP.getFreeHeap()`, `ESP.getMinFreeHeap()`,
  `ESP.getMaxAllocHeap()`.

**`src/telegram.cpp`**:
- Register `/heap` command with description "Show free heap stats".
- Update the Serial log string to include `/heap`.

## Notes for handover

Changed: `src/telegram_poller.{h,cpp}`, `src/main.cpp`,
`src/telegram.cpp`, `rfc/0072-heap-command.md`.

No new native tests: heapFn_ follows the identical callback pattern
as statusFn_, concatSummaryFn_, etc.
