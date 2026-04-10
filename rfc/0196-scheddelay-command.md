---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0196: `/scheddelay <N> <extra_min>` — extend scheduled SMS deadline

## Motivation

When a scheduled SMS was set with `/schedulesend` and the operator wants to
push back the send time (e.g. "actually send 15 min later than originally
scheduled"), they currently have to cancel and re-schedule. `/scheddelay`
adds minutes to an existing slot's deadline without losing the phone/body.

## Plan

### TelegramPoller: `/scheddelay <N> <extra_min>`

- `N` is the 1-based slot number (as shown in `/schedqueue`).
- `extra_min` is the number of minutes to add (1–1440).
- Adds `extra_min * 60000` ms to the slot's `sendAtMs`.
- Replies: "✅ Slot N deadline extended by M min."
- Error if slot is empty or out of range.
