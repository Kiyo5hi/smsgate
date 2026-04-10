---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0063: /time command

## Motivation

`/status` contains the current time but also several screens of other
information. `/time` gives the current UTC time in a single message — useful
as a quick NTP sanity check without reading through a wall of stats.

## Plan

**`src/telegram_poller.cpp`**:
- Add handler between `/ping` and `/last`:
  ```cpp
  if (lower == "/time") {
      // format time(nullptr) as YYYY-MM-DD HH:MM UTC
      // if no NTP sync: "(no NTP sync yet)"
  }
  ```
- Add `/time` entry to `/help`.

**`src/telegram.cpp`**:
- Register `/time` command (before `/ntp`).
- Update Serial log line.

## Notes for handover

Changed: `src/telegram_poller.cpp`, `src/telegram.cpp`. No test changes
needed.
