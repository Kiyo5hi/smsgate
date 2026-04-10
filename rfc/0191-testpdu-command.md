---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0191: `/testpdu <hex>` — decode a raw PDU hex string

## Motivation

When debugging modem integration (e.g., comparing raw AT+CMGR output with
what the firmware decodes), it's useful to paste a raw PDU hex string into
Telegram and see the decoded fields without needing a serial monitor.

## Plan

### TelegramPoller command: `/testpdu <hex>`

- Takes a single hex string argument (no spaces required between hex digits).
- Strips all whitespace from the argument, passes to `sms_codec::parseSmsPdu`.
- On success, replies with:
  ```
  📨 PDU decoded:
  Sender: <sender>
  Time: <rfc3339 timestamp>
  Body: <content>
  [Concat: ref=<n> part=<m>/<total>]
  ```
- On failure (malformed PDU), replies: "❌ Failed to parse PDU."
- No fn setter needed — `parseSmsPdu` is pure and already available.
- Range: up to 500 chars of hex (≈250 bytes = 2 × 140-byte SMS with concat UDH).

### Persistence

None — purely stateless decode.

## Notes for handover

- This command is purely diagnostic and does not send or delete anything.
- The timestamp is formatted via `sms_codec::timestampToRFC3339` using the
  handler's current GMT offset (passed via a getter fn or just hardcoded
  to the poller's stored gmtOffsetMinutes — simplest: make a `getGmtOffsetFn_`
  getter, or just use 0 and note UTC in the reply).
- Simplest implementation: format using the GMT offset stored in the handler
  exposed via `SmsHandler::gmtOffsetMinutes()` passed through a setter lambda.
