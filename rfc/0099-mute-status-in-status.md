---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0099: Alert mute state in /status

## Motivation

After /mute, the operator has no way to check whether alerts are muted
or how much time remains without waiting for the snooze to expire. Adding
a one-liner to the Config section of /status makes the mute state visible.

## Design

In the statusFn Config section in main.cpp, after the block list line:

    if muted:    "  Alerts: muted (Xm remaining)"
    if unmuted:  (nothing — normal state, no need to show)

## File changes

**`src/main.cpp`** — add mute state line in statusFn Config section
