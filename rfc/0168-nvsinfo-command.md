---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0168: /nvsinfo command — NVS storage usage statistics

## Motivation

The ESP32's NVS (Non-Volatile Storage) partition has a fixed number of entries.
As the reply-target ring buffer and other persistent data grow, knowing how many
NVS entries are free vs. used helps diagnose "NVS full" failures at the
composition root. `/nvsinfo` calls `nvs_get_stats()` and reports the counts.

## Plan

Add `setNvsInfoFn(std::function<String()>)` to TelegramPoller. Add `/nvsinfo`
handler. Wire in main.cpp with a lambda that calls `nvs_get_stats(NULL, &stats)`
and formats `used_entries / total_entries` with percentage. Add to setMyCommands.
