---
status: proposed
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0003: Bidirectional bridge — Telegram → SMS

## Motivation

Right now the bridge is one-way: SMS → Telegram. The natural extension
is to let the user reply to a forwarded message in Telegram and have the
bot send the reply back over SMS to the original sender. This makes the
device useful as a remote SIM for someone whose primary phone is
elsewhere (the original use case).

## Dependencies

- **Hard: RFC-0001 must be `implemented` before this RFC is started.**
  "Accepted" or "in-progress" is not enough. Until TLS actually verifies
  the server, an attacker on the bridge's LAN can inject fake Telegram
  updates over `setInsecure()` and turn this device into a free SMS
  relay. This risk exists today for SMS→TG exfiltration too, but once
  we add the TG→SMS direction it becomes an *active* attack surface,
  not a passive one.
- **Hard: RFC-0007 (testability) must be `implemented` first.** The
  long-polling loop, `update_id` tracking, `reply_to_message_id` map
  lookup, and authorization gate are all stateful logic that we cannot
  responsibly ship if every regression test requires reflashing the
  board. Design the new `TelegramPoller` / `SmsSender` modules as
  classes depending on `IBotClient` and `IModem` from the start.
- **Soft: overlaps with RFC-0002 on encoding.** Sending non-ASCII SMS
  requires either PDU mode or `AT+CSMP=17,167,0,8` + `+CSCS="UCS2"` in
  text mode. TinyGSM already handles the text-mode UCS2 path via
  `sendSMS_UTF16Impl` (see `TinyGsmSMS.tpp:205`). Prefer that for the
  first cut; reconsider once 0002 lands.

## Current state

Not started. The code is structured around `loop()` reading SMS URCs only.

## Plan

1. **Telegram receive.** Telegram bots have two delivery models:
   - **Long polling** via `getUpdates`. Simple, no public IP required.
     Costs one HTTPS request every N seconds. Fits this device.
   - **Webhooks.** Requires the bridge to be reachable from the
     internet. Out of scope.

   Use long polling. Add a `pollTelegramUpdates()` called from `loop()`
   on a timer (e.g. every 3 seconds). Track the last `update_id` we
   processed in RTC memory so we don't replay across reboots.

2. **Reply targeting.** When we forward an inbound SMS we need to embed
   the sender's phone number in the Telegram message in a way that
   survives the round-trip and resists user-side editing. Two options:
   - Use Telegram's `reply_to_message_id`: when the user replies in the
     thread, the API tells us which message they replied to. Maintain a
     small map (message_id → phone number) in NVS so we can look up the
     destination.
   - Encode the phone number in a hidden bot command suffix
     (`#+861234...`). Brittle; users edit it out.

   Prefer the first option. **Eviction is a ring buffer keyed by
   `message_id % 200`**, since Telegram hands out monotonically
   increasing `message_id`s per chat. That means each new forwarded
   message overwrites whatever was in slot `(message_id % 200)`,
   giving us an implicit "last 200 entries" window without any LRU
   bookkeeping. The slot stores both `{message_id, phone_number}`, so
   when we look up a reply we can confirm the stored `message_id`
   matches before trusting the phone number (otherwise the slot has
   been reused by a newer message and the reply is too old to route).

3. **Sending SMS.** For the first cut, use
   `modem.sendSMS(phone, body)` — the plain ASCII path that goes
   through `sendSMSImpl` — and **bail on non-ASCII bodies** with a
   Telegram error reply ("non-ASCII SMS replies not yet supported —
   waiting on RFC-0002"). This keeps the first cut small.

   For full Unicode support, use `modem.sendSMS_UTF16`. Note its
   signature: `sendSMS_UTF16(const char* number, const void* text,
   size_t len)` — it takes a raw UTF-16BE byte buffer plus its
   length, **not** an Arduino `String`. See `TinyGsmSMS.tpp:28`. The
   implementor must convert the reply body from its source encoding
   (UTF-8 from the Telegram update) to a UTF-16BE byte buffer before
   calling, which is ~15 lines of conversion code (code-point
   decode → surrogate pair expansion → big-endian byte pack), not a
   one-liner. Either write that conversion inline in `SmsSender` or
   share it with the PDU encoder once RFC-0002 lands.

4. **Authorization.** Restrict which Telegram users can send SMS.
   Re-use the existing `TELEGRAM_CHAT_ID` as the allow-list (single
   user) for the first cut. Multi-user later.

5. **Fail-closed update parsing.** If JSON parsing of a polled update
   fails (malformed response, unexpected shape, truncated stream,
   etc.), drop the update silently: do not forward to SMS, do not
   retry the same `update_id`, advance past it (i.e. use the
   `update_id` from the response envelope rather than re-requesting
   the same offset). Log a single line at `WARN` level and move on.
   The pipeline never blocks on a bad update, and we never accidentally
   send an SMS from half-parsed user input.

## Notes for handover

- Do not implement webhook mode.
- Long polling timeout: pass `timeout=25` so the request blocks on the
  Telegram side. This drops poll frequency from "every 3s" to "as fast
  as Telegram releases", which is much friendlier to data caps and
  battery.
- The Telegram getUpdates response can be large; bump the
  `DynamicJsonDocument` size or use `ArduinoJson::deserializeJson` with
  a stream filter to extract just `result[*].message.text` and
  `result[*].message.reply_to_message.message_id`.
- `IBotClient` (from RFC-0007) needs an extra method for this RFC:
  something like `bool pollUpdates(int32_t sinceUpdateId, std::vector<TelegramUpdate> &out)`.
  Add that method to the interface as part of *this* RFC's
  implementation, not 0007's — 0007 only has to cover what SMS→TG
  needs today.
- Reboot-persistence of the `message_id → phone number` map and the
  last-seen `update_id` should use NVS / Preferences, not RTC memory,
  because the current failure-recovery path is `ESP.restart()` which
  clears RTC if power dropped.
- Make sure RFC-0001 is `implemented` (real TLS verified on the
  deployment network) before shipping bidirectional — otherwise an
  attacker on the bridge's network could inject fake Telegram updates
  and use this device as a free SMS relay.
