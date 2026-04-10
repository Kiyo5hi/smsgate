---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0133 — /shortcuts command

## Motivation

The full `/help` output is long and overwhelming for new operators.
`/shortcuts` shows only the 10 most-used commands as a quick reference.

## Plan

1. Add `/shortcuts` handler to `TelegramPoller`. Returns a hardcoded
   compact cheat sheet: `/ping`, `/status`, `/send`, `/queue`, `/last`,
   `/network`, `/uptime`, `/count`, `/reboot`, `/help`.

2. No setter needed — static content.

3. Tests:
   - `/shortcuts` replies with a message containing key command names.
