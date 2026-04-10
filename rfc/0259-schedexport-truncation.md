---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0259: Truncate /schedexport output at Telegram's 4096-char limit

## Motivation

`/schedexport` builds one command line per occupied slot and sends the
result as a single `bot_.sendMessageTo()` call.

Each slot's command line is:
- `/scheduleat 2026-04-10 14:32 <phone> <body>\n` — header ~33 chars +
  phone ~15 chars + body up to 1530 chars (10 × 153 GSM-7) = ~1578 chars
- `/recurring <min> <phone> <body>\n` — similar

With `kScheduledQueueSize = 5` and all slots holding max-length bodies:
5 × 1578 = **7890 chars** — nearly 2× Telegram's 4096-char limit.

If the output exceeds 4096 chars, `sendMessageTo()` fails silently:
Telegram returns `"ok":false` and the user gets no response.  For
`/schedexport` specifically, a silent failure is particularly harmful
because the user is trying to back up their schedule and receives no
indication that only part of it was exported.

## Plan

Build slot lines one at a time. After each line, check whether adding
it would push the total past a safety threshold (~3900 chars, leaving
room for the header and footer). If it would, stop and append a notice:

```
(N slot(s) shown, M omitted — use /schedinfo for long-body slots)
```

Uses `String::length()` and `operator+=` — both available in the
native test stub.

## Notes for handover

- The header `"\xf0\x9f\x93\x8b Scheduled queue export (N slot(s)):\n"`
  is prepended to the slot lines at the send site, so the threshold must
  account for it (~40 chars).
- This is analogous to RFC-0258 (`dump()` truncation) but uses an
  early-exit strategy (stop adding entries) rather than post-hoc
  truncation, so the output lines are always complete.
- `/schedinfo <N>` is the recommended fallback for slots with long bodies.
