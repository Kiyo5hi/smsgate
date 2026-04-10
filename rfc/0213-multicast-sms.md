---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0213: `/multicast` — send the same SMS to multiple phone numbers

## Motivation

Operators sometimes need to send an identical alert or announcement to
a handful of contacts (e.g. family members, on-call engineers). The
current workflow requires one `/send` per recipient, which is tedious
and produces multiple separate confirmation messages.

## Design

New command: `/multicast <phone1,phone2,...> <body>`

- Phone numbers are comma-separated (no spaces around commas required).
- Body follows the last phone number after a single space.
- Each number is normalized via `sms_codec::normalizePhoneNumber`.
- Up to 10 recipients per call (excess are rejected with an error).
- Each recipient is enqueued via `SmsSender::enqueue` independently.
- Body is subject to the same 10-part length limit as `/send`.

Reply format on success:
```
✅ Multicast queued to 3 numbers: +1111, +2222, +3333
```

On partial normalization failures (empty phone after normalize):
```
⚠️ Skipped 1 invalid number(s). Queued to 2: +1111, +3333
```

If all numbers are invalid or body is empty: error reply, nothing queued.

## Notes for handover

- Max 10 recipients hard-coded as `kMulticastMaxRecipients = 10`.
- No new subsystem APIs — pure `SmsSender::enqueue` calls.
- The delivery confirmation callback (onSuccess) and failure callback
  (onFinalFailure) from RFC-0012/0032 are passed per-enqueue so each
  recipient gets independent retry and result notification.
- Reply targeting: each enqueue fires `sendMessageReturningId` on
  success and stores (message_id, phone) in ReplyTargetMap, allowing
  replies to any of the delivery confirmations to route back.
