---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0135 — /status shows device note when set

## Motivation

The device note (RFC-0131) is stored in NVS but currently not displayed
anywhere except via the `/note` command. Surfacing it in `/status` makes
it immediately visible during routine health checks.

## Plan

1. In `main.cpp`, add the device note to the `statusFn` lambda output
   when `s_deviceNote` is non-empty:
   ```
   Note: SIM changed 2026-04-10
   ```

2. No new tests needed — the statusFn is wired in setup() and the note
   is a file-static string. Covered by manual verification.
