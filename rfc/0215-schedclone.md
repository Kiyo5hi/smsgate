---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0215: `/schedclone <N> <min>` — duplicate a scheduled slot

## Motivation

An operator wants to send the same SMS to the same number again at a
different time (e.g. a reminder). Currently they must retype the full
body with `/schedulesend`. `/schedclone` copies the phone + body of
an existing slot into a new free slot with a fresh delay.

## Design

New command: `/schedclone <N> <delay_min>`

- `N` — source slot (1-based), must be occupied.
- `delay_min` — delay in minutes (1–1440) for the new clone slot.
- A free slot must exist (queue not full).
- The source slot is NOT cancelled — both slots stay active.

On success:
```
✅ Slot 1 cloned → slot 3 (fires in 30m).
```

## Notes for handover

- Identical flow to `/schedulesend` after the source slot is read.
- Uses same `schedEtaStr` + `quietHoursWarning` as `/schedulesend`.
- `persistSchedFn_` called after mutation.
- 1 native test: clone slot 1 → verify slot 3 occupied with same
  phone+body.
