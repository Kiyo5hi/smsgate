---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0205: `/sendafter HH:MM <phone> <body>` — schedule SMS at a clock time

## Motivation

`/schedulesend 45 +1234 Hello` requires mental math. `/sendafter 14:30
+1234 Hello` lets the operator schedule for a specific clock time
without computing the delta manually.

## Design

Command: `/sendafter <HH:MM> <phone> <body>`

- `HH:MM` is interpreted as UTC time today; if the time is already past,
  it schedules for tomorrow (same time).
- Requires NTP to be synced (`wallTimeFn_` must return > 1e9). If not,
  reply: "⚠️ NTP not synced. Use /schedulesend <min> instead."
- Computes `delayMin = (targetUnix - now + 59) / 60` and inserts into
  the scheduled queue exactly like `/schedulesend`.
- Uses `wallTimeFn_` (set by RFC-0202) to get current wall time; no
  new setter needed.
- Call `persistSchedFn_()` after inserting.

## Notes for handover

- Parse: split on first space to get `HH:MM`, then second space to
  split phone from body (same as `/schedulesend`).
- Valid HH: 0–23, valid MM: 0–59.
- If computed delayMin is 0 (time is within the next 60s), clamp to 1.
- Max delayMin is 1440 (24h); if computed delay > 1440, the target
  time is more than 24h away — accept it since it will just wrap to
  "tomorrow" or "day after tomorrow".
- Actually there's no 1440 cap: if it's 14:30 UTC and you do
  `/sendafter 14:29`, it schedules 23h59m from now — fully valid.
  Remove the 1440 cap for `/sendafter`.
