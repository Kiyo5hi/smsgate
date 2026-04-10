---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0070: Multi-user SMS forwarding

## Motivation

RFC-0014 added a multi-user allow list for bot commands, but incoming SMS
were still only forwarded to the admin (allowedIds[0]). When multiple users
are in the allow list, all of them should receive forwarded SMS — a common
family/team scenario.

## Plan

**`src/sms_handler.h`**:
- Add `void setExtraRecipients(const int64_t *ids, int count)` setter.
- Add `const int64_t *extraRecipients_; int extraRecipientCount_;` members.

**`src/sms_handler.cpp`**:
- In `forwardSingle`: after admin send + reply-target put, call
  `bot_.sendMessageTo(extraRecipients_[i], formatted)` for each extra user.
- In `insertFragmentAndMaybePost`: same fan-out pattern.

**`src/main.cpp`**:
- After parsing allow list: `smsHandler.setExtraRecipients(allowedIds + 1,
  allowedIdCount - 1)` (skips admin at index 0, who already gets the message
  via `sendMessageReturningId`).

## Reply routing

Reply-routing (via the reply-target ring) only works for the admin, since
only the admin's message_id is stored. Extra recipients see the SMS but
their replies are treated as new commands (not reply-routed to the SMS
sender) unless they reply to the admin's forwarded message somehow.

## Notes for handover

Changed: `src/sms_handler.{h,cpp}`, `src/main.cpp`,
`test/test_native/test_sms_handler.cpp` (1 new test).
