---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0067: /echo command + /cleardebug entry count

## Motivation

Two small UX improvements:

1. `/echo <text>` reflects back whatever text is sent. Useful as a
   round-trip connectivity test (verifies both receive and send paths work
   without needing to wait for a real SMS).

2. `/cleardebug` previously responded with a generic "cleared" message.
   Reporting the number of entries cleared ("Cleared 5 entries") is more
   informative.

## Plan

**`src/telegram_poller.cpp`**:
- `/cleardebug`: capture `debugLog_->count()` before `clear()`, include
  count in response.
- New handler before `/status`:
  ```cpp
  if (lower.startsWith("/echo")) {
      bot_.sendMessageTo(u.chatId, arg.length() > 0 ? arg : "(empty)");
  }
  ```
- `/help`: add `/echo` entry.

**`src/telegram.cpp`**:
- Register `/echo` command, update Serial log.

## Notes for handover

Changed: `src/telegram_poller.cpp`, `src/telegram.cpp`. No test changes
needed.
