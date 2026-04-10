---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0123 — /boot command

## Motivation

The operator wants a quick view of the boot history — boot count,
reset reason, and boot timestamp — without the full `/status` wall
of text.

## Plan

1. Add `setBootInfoFn(std::function<String()> fn)` setter to
   `TelegramPoller`. When set, `/boot` calls this fn and replies with
   the result. When not set, replies "(boot info not configured)".

2. In `main.cpp`, wire a lambda returning:
   ```
   🔄 Boot #42 | Reason: WDT | 2026-04-10 14:32 UTC
   ```

3. Tests:
   - `/boot` with fn set → replies with fn result.
   - `/boot` without fn → replies "(boot info not configured)".
