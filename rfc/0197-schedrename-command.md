---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0197: `/schedrename <N> <phone>` — change scheduled slot phone number

## Motivation

When a scheduled SMS was set with the wrong phone number, the operator
currently has to cancel and re-schedule with the right number.
`/schedrename` changes only the destination phone, preserving the body
and the original timing.

## Plan

### TelegramPoller: `/schedrename <N> <phone>`

- `N` is the 1-based slot number (as shown in `/schedqueue`).
- `phone` is the new destination (normalized via `normalizePhoneNumber`).
- Replies: "✅ Slot N phone changed to <phone>."
- Error if slot is empty or out of range; error if phone is empty.
