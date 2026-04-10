---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0127 — /smsslots command

## Motivation

The `/status` command shows SIM slot usage in its long wall-of-text.
A dedicated `/smsslots` gives the operator an instant slot-usage
one-liner without reading through everything else.

## Plan

1. Add `setSmsSlotssFn(std::function<String()> fn)` setter to
   `TelegramPoller`. When set, `/smsslots` calls this fn and replies
   with the result. When not set, replies "(SMS slots info not configured)".

2. In `main.cpp`, wire a lambda:
   ```
   📨 SIM slots: 5/30 used
   ```

3. Tests:
   - `/smsslots` with fn set → replies with fn result.
   - `/smsslots` without fn → replies "(SMS slots info not configured)".
