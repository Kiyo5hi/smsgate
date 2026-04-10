---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0130 — /digest command

## Motivation

The daily digest fires automatically every 24 hours. The operator
sometimes wants to see the digest immediately without waiting for
the scheduled window.

## Plan

1. Add `setDigestFn(std::function<String()> fn)` setter to
   `TelegramPoller`. When set, `/digest` calls this fn and replies
   with the result. When not set, replies "(digest not configured)".

2. In `main.cpp`, wire a lambda that formats the same content as the
   daily digest (session SMS count, forwarded, failed, outbound, calls,
   uptime).

3. Tests:
   - `/digest` with fn set → replies with fn result.
   - `/digest` without fn → replies "(digest not configured)".
