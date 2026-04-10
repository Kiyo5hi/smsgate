---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0121 — /network command

## Motivation

The operator sometimes needs a quick view of the cellular registration
state and operator name without the full `/status` wall of text.
`/network` gives a compact snapshot: operator name, registration status,
and CSQ.

## Plan

1. Add `setNetworkFn(std::function<String()> fn)` setter to
   `TelegramPoller`. When set, `/network` replies with the fn result.
   When not set, replies "(network info not configured)".

2. In `main.cpp`, wire a lambda:
   ```
   📶 Operator: T-Mobile | Reg: home | CSQ 18 (good)
   ```

3. Tests:
   - `/network` with fn set → replies with fn result.
   - `/network` without fn → replies "(network info not configured)".
