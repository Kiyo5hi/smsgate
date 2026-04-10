---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0181: /fwdtest command — preview forwarded SMS format

## Motivation

After changing `/setfwdtag`, `/setgmtoffsetmin`, or `/addalias`, operators
want to preview what a real forwarded SMS will look like without waiting for
one to arrive. `/fwdtest` sends a synthetic forward to the chat using
fake data so the format is immediately visible.

## Plan

1. **`TelegramPoller`** — add `setFwdTestFn(std::function<String()> fn)`.
   The fn calls `smsHandler.formatBotMessage(...)` (or equivalent) with:
   - Sender: "+10000000000"
   - Timestamp: a recent PDU timestamp string derived from `time(nullptr)`
   - Body: "This is a test message. / 这是测试消息。"
   Returns the formatted message string.

2. `/fwdtest` handler calls `fwdTestFn_()` and sends it to the chat.

3. **`main.cpp`** — wire the lambda. Uses `smsHandler.gmtOffsetMinutes()`,
   `smsHandler.fwdTag()`, and `smsHandler.aliasFn_` (via a test phone that
   won't have an alias, showing raw format). Builds the timestamp string from
   `time(nullptr)`.

   Actually the cleanest approach: expose `SmsHandler::formatBotMessageForTest()`
   as a public method taking a fake sender/body and using the current gmtOffset
   and fwdTag settings.

4. **Tests** — one poller test: `/fwdtest` calls fn and sends result.

## Notes for handover

- The synthetic timestamp must match PDU format "YY/MM/DD,HH:MM:SS+tz" to be
  parsed by `timestampToRFC3339`. Build it from `time(nullptr)`.
- The test phone "+10000000000" won't match any real alias, showing raw format.
- Command is intentionally lightweight: no modem access, no SIM, no NVS.
