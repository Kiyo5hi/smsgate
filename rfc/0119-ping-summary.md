---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0119 — /ping enhanced with brief summary

## Motivation

The current `/ping` just replies "🏓 Pong". For a quick health check the
operator also wants uptime, CSQ, and device label on the same line, so
they don't have to issue a second `/status` command just to verify the
device is alive and well.

## Plan

1. Add `setPingSummaryFn(std::function<String()> fn)` setter to
   `TelegramPoller`. When set, `/ping` uses the fn result as the reply.
   When not set, falls back to "🏓 Pong" (existing behavior).

2. In `main.cpp`, wire a lambda that returns a compact one-liner:
   ```
   🏓 Pong [Office SIM] | ⏱ 2d 3h 15m | CSQ 18 (good)
   ```
   Includes: device label (if set), uptime, CSQ label.

3. Tests:
   - `/ping` with fn set → replies with fn result.
   - `/ping` without fn → replies "Pong".
