---
status: proposed
created: 2026-04-09
---

# RFC-0003: Bidirectional bridge — Telegram → SMS

## Motivation

Right now the bridge is one-way: SMS → Telegram. The natural extension
is to let the user reply to a forwarded message in Telegram and have the
bot send the reply back over SMS to the original sender. This makes the
device useful as a remote SIM for someone whose primary phone is
elsewhere (the original use case).

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

   Prefer the first option. Bound the map (e.g. last 200 entries) to
   keep NVS bounded.

3. **Sending SMS.** Use `modem.sendSMS(phone, body)`. Note that text
   mode SMS send has the same encoding constraints as receive (see
   RFC-0002), so there is overlap with the PDU work — sending UCS2
   from text mode requires `AT+CSMP` setup. Test with English first,
   then Chinese.

4. **Authorization.** Restrict which Telegram users can send SMS.
   Re-use the existing `TELEGRAM_CHAT_ID` as the allow-list (single
   user) for the first cut. Multi-user later.

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
- Make sure RFC-0001 is closed (real TLS) before shipping bidirectional —
  otherwise an attacker on the bridge's network could inject fake
  Telegram updates and use this device as a free SMS relay.
