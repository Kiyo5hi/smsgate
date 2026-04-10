---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0069: /concat command — in-flight concat reassembly status

## Motivation

When a multi-part SMS isn't arriving, there's no way to see whether the
fragments are buffered in the reassembly ring. `/concat` surfaces this
without needing serial access.

## Plan

**`src/sms_handler.h`**: Add `String concatGroupsSummary() const;`

**`src/sms_handler.cpp`**: Implement — lists each in-flight group:
`ref=0xNN sender — X/N parts  Y B`

**`src/telegram_poller.h`**: Add `setConcatSummaryFn(std::function<String()>)`
setter + `concatSummaryFn_` member.

**`src/telegram_poller.cpp`**: Add `/concat` handler; update `/help`.

**`src/telegram.cpp`**: Register `/concat` command; update Serial log.

**`src/main.cpp`**: Wire `telegramPoller->setConcatSummaryFn(...)`.

## Notes for handover

Changed: `src/sms_handler.{h,cpp}`, `src/telegram_poller.{h,cpp}`,
`src/telegram.cpp`, `src/main.cpp`. No test changes needed.
