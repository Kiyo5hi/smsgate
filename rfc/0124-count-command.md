---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0124 — /count command

## Motivation

The operator sometimes wants a quick view of how many SMS and calls
have been received/forwarded this session without the full `/status`
wall of text.

## Plan

1. Add `setCountFn(std::function<String()> fn)` setter to
   `TelegramPoller`. When set, `/count` calls this fn and replies with
   the result. When not set, replies "(count not configured)".

2. In `main.cpp`, wire a lambda returning:
   ```
   📊 SMS: rcvd 12 | fwd 11 | fail 1 | Calls: 3
   ```

3. Tests:
   - `/count` with fn set → replies with fn result.
   - `/count` without fn → replies "(count not configured)".
