---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0131 — /note and /setnote commands

## Motivation

The operator sometimes needs to leave a persistent note on the device
(e.g., "SIM changed 2026-04-10", "Office device — do not reboot").
`/setnote <text>` saves it to NVS; `/note` retrieves it.

## Plan

1. Add `setNoteGetFn` and `setNoteSetFn` setters to `TelegramPoller`
   (analogous to the existing label get/set pattern from RFC-0118).

2. In `main.cpp`, wire lambdas that load/save a "device_note" key in
   NVS. Max 120 chars.

3. Tests:
   - `/note` with fn → replies with current note.
   - `/note` with empty note → "(no note set)".
   - `/setnote <text>` calls setter and replies "✅ Note saved.".
   - `/setnote` with no arg → usage error.
   - `/setnote` too long → error.
