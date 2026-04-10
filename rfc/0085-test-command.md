---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0085: /test command — outbound SMS self-test

## Motivation

After a firmware update or connectivity change the operator wants to
verify the full outbound SMS path without manually composing a `/send`.
A `/test <number>` command sends a fixed test message ("Bridge test OK
<timestamp>") and reports the result, giving a quick go/no-go signal.

## Plan

**`src/telegram_poller.cpp`**:
- Add `/test` handler (admin-only via the auth gate that's already in
  processUpdate). Extract phone number from arg (one word), normalize
  it via `sms_codec::normalizePhoneNumber`. If no number given, send
  usage hint.
- Enqueue via `smsSender_.enqueue(phone, body, failCb, okCb)` where:
  - body = "Bridge test OK " + current UTC time (or "(no NTP)" if clock invalid)
  - failCb = send "❌ Test SMS to <phone> failed."
  - okCb = send "✅ Test SMS to <phone> sent."
- Add `/test <num> — Send test SMS to verify outbound path` to `/help`.

**`src/telegram.cpp`**:
- Register `/test` command with description "Send a test SMS".
- Update Serial log string.

## Notes for handover

Changed: `src/telegram_poller.{cpp}`, `src/telegram.cpp`,
`rfc/0085-test-command.md`.

No new native tests — the /send path which this reuses is already tested.
