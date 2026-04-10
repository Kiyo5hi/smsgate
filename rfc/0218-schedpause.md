---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0218: `/schedpause` / `/schedresume` — globally pause scheduled delivery

## Motivation

Sometimes an operator wants to hold all scheduled SMS (e.g. network
outage, wrong time zone realization) without cancelling individual
slots. Quiet hours is automatic; this is manual on-demand pause.

## Design

New commands:
- `/schedpause` — set `schedPaused_ = true`; replies "⏸ Scheduled SMS delivery paused."
- `/schedresume` — set `schedPaused_ = false`; replies "▶️ Scheduled SMS delivery resumed."

`tick()` guard: `if (schedPaused_) skip scheduled drain`.

`/schedqueue` appends "⏸ (delivery paused)" header when flag is set.

`/status` shows "(paused)" next to sched count when flag is set.

No NVS persistence — intentionally volatile. A reboot auto-resumes.

## Notes for handover

- New bool member `schedPaused_ = false` on TelegramPoller.
- Quiet hours check (`inQuiet`) is separate from `schedPaused_` —
  both independently suppress firing. Either suppresses delivery.
- Add to `/help`.
- 2 native tests: (a) pause suppresses tick fire; (b) resume allows it.
