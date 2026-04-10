---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0212: Warn when a new scheduled SMS overlaps with quiet hours

## Motivation

An operator sets `/setquiethours 22-08` and then runs
`/schedulesend 30 +1234567890 Reminder`. If the current time is
21:55 UTC, the SMS will be scheduled for 22:25 UTC — inside the quiet
window — and will be silently deferred until 08:00 UTC (9+ hours later).
Without a warning the operator will be confused why the SMS didn't arrive.

## Design

After any successful scheduling command (`/schedulesend`, `/sendafter`,
`/schedrename` does NOT apply — phone not time), compute whether the
`sendAtMs` will fall inside the quiet window using `isInQuietHours` and
a projected wall-clock time. If yes, append to the confirmation reply:

```
⚠️ Note: this slot falls inside quiet hours (22:00-08:00 UTC) and will
be deferred until 08:00 UTC.
```

`isInQuietHoursAt(sendAtUnix, start, end)` — separate helper that takes
a unix timestamp directly rather than consulting `wallTimeFn_` for "now".
Reuses the same same-day / overnight logic.

Commands that need the check:
- `/schedulesend` — already has sendAtMs at confirmation time
- `/sendafter` — already has sendAtMs at confirmation time

Commands that adjust an existing slot (`/scheddelay`, `/delayall`,
`/sendnow`) — also add the overlap check so the user knows the adjusted
time still falls in quiet hours.

## Notes for handover

- `isInQuietHoursAt(long unixTs, int start, int end)` is a file-static
  helper (no wallTimeFn needed — takes the absolute time directly).
- The warning is appended to the confirmation String before `sendMessageTo`.
- No new NVS keys, no new members, no new tests beyond the existing
  RFC-0211 tests which already verify suppression logic.
- One new native test: schedule a slot during quiet window and assert
  confirmation contains "quiet hours".
