---
status: proposed
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0272: "retry" keyword shortcut to resend last failed SMS

## Motivation

When an outbound SMS fails permanently (all retries exhausted), the user
receives a failure notification.  To resend they must go back to the original
forwarded SMS and reply again.  A "retry" shortcut reply to the failure message
would eliminate this friction.

## Plan

When the `onFinalFailure` callback fires in `TelegramPoller::processUpdate()`:

1. Send the failure notification via `sendMessageReturningId()` so we get its
   `message_id`.
2. Call `replyTargets_.put(failMsgId, phone)` to register the failure message
   as a valid reply target.
3. Append "Reply with the new message text to resend." to the failure notification.
4. In the reply-target dispatch path, before passing the text to `SmsSender`,
   check if `u.text` is empty after trimming — if so, reject with a helpful error.

The user's reply to the failure message then routes through the normal
`replyTargets_` lookup and `SmsSender::enqueue()` path unchanged.

## Open questions

- Does "reply to failure message" conflict with sending an SMS that
  literally starts with the same text as the failure notification?  (No —
  the reply-target lookup is by `reply_to_message_id`, not text content.)
- Should the failure message explicitly say "reply with new text" rather than
  implying a "retry" keyword?  The simpler UX is probably just showing that
  the failure message is itself a valid reply target.

## Notes for handover

- This reuses `replyTargets_` for its intended purpose; no new state needed.
- The `sendMessageReturningId` failure path in `processUpdate()` currently uses
  `sendMessage` (bool variant).  Change to `sendMessageReturningId` and store
  the id.
- Low priority relative to stability fixes.
