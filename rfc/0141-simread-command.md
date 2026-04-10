---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0141: /simread <idx> command — read a specific SIM slot

## Motivation

Complement to RFC-0140. Once `/simlist` shows what indices exist,
`/simread <idx>` retrieves and decodes the full SMS content — useful
for inspecting stuck concat fragments or unforwarded messages.

## Plan

Add `setSimReadFn(std::function<String(int)>)` setter to `TelegramPoller`.
The fn issues AT+CMGR=<idx>, decodes the PDU via `sms_codec::parseSmsPdu`,
and returns a formatted string: sender, timestamp, body.

In `main.cpp` wire it via `realModem.sendAT("+CMGR=<idx>")` and parse
using the existing `parseCmgrBody` + `parseSmsPdu` pipeline.

## Notes for handover

Use the existing `SmsHandler::handleSmsIndex` logic as a template — the
difference is we return the formatted string instead of forwarding to
Telegram. Validate `idx` range (1–255) before sending to modem.
