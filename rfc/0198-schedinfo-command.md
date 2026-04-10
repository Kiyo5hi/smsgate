---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0198: `/schedinfo <N>` — show full content of a scheduled slot

## Motivation

`/schedqueue` truncates the body preview at 40 characters. For long
scheduled messages, operators need to see the full body to verify
it before it fires. `/schedinfo 1` shows the complete content of slot N.

## Plan

### TelegramPoller: `/schedinfo <N>`

- `N` is the 1-based slot number.
- Replies with the full slot content: phone, ETA, and full body.
- Error if slot is empty or out of range.
