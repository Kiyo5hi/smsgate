---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0120 — /uptime command

## Motivation

`/status` returns a multi-line wall of text. Often the operator just
wants to know how long the bridge has been up without wading through
CSQ, counters, NTP time, etc. A dedicated `/uptime` gives them a
one-liner instantly.

## Plan

1. Add `setUptimeFn(std::function<String()> fn)` setter to
   `TelegramPoller`. When set, `/uptime` replies with the fn result.
   When not set, replies "(uptime not configured)".

2. In `main.cpp`, wire a lambda:
   ```
   ⏱ 2d 3h 15m 42s
   ```

3. Tests:
   - `/uptime` with fn set → replies with fn result.
   - `/uptime` without fn → replies "(uptime not configured)".
