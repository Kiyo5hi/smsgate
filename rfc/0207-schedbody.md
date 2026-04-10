---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0207: `/schedbody <N> <new_body>` — edit the body of a scheduled SMS

## Motivation

`/schedrename <N> <phone>` lets you change a scheduled SMS's destination.
There's no symmetric command to edit the body without cancelling and
re-creating the slot (losing the original ETA).

## Design

Command: `/schedbody <N> <new_body>`

- `N` is the 1-based slot index.
- `new_body` replaces the current body (trimmed; truncated at 127 chars).
- On success: `"✅ Slot N body updated. New body: <preview>..."`
- Validates: N in 1–5, slot occupied. Same error messages as /schedrename.
- Calls `persistSchedFn_()` after mutation.
- Adds to `/help`.

## Notes for handover

- Pattern: identical to `/schedrename` but mutates `body` instead of `phone`.
- If new_body is empty, reply with usage.
