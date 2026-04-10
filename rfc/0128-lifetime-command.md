---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0128 — /lifetime command

## Motivation

The operator wants to see long-term device health metrics — lifetime
SMS count and total boot count — without reading through `/status`.

## Plan

1. Add `setLifetimeFn(std::function<String()> fn)` setter to
   `TelegramPoller`. When set, `/lifetime` calls this fn and replies
   with the result. When not set, replies "(lifetime stats not configured)".

2. In `main.cpp`, wire a lambda:
   ```
   📈 Lifetime: 1234 SMS forwarded | Boot #42
   ```

3. Tests:
   - `/lifetime` with fn set → replies with fn result.
   - `/lifetime` without fn → replies "(lifetime stats not configured)".
