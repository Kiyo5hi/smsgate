---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0137 — /setinterval command

## Motivation

The heartbeat interval is currently a compile-time constant
(`HEARTBEAT_INTERVAL_SEC`, default 6 hours). The operator may want to
change it at runtime — e.g., increase frequency during troubleshooting
or disable it entirely without reflashing.

## Plan

1. Add `setIntervalFn(std::function<void(uint32_t)> fn)` setter to
   `TelegramPoller`. When set, `/setinterval <seconds>` validates
   the argument (0 = disable, max 86400 = 24h) and calls this fn.
   When not set, replies "(setinterval not configured)".

2. In `main.cpp`, wire a lambda that updates `HEARTBEAT_INTERVAL_SEC`
   via a runtime-writable static `s_heartbeatIntervalSec` and persists
   to NVS key "hb_interval".

3. Tests:
   - `/setinterval 3600` calls fn with 3600.
   - `/setinterval 0` calls fn with 0 (disable).
   - `/setinterval` with no arg → usage error.
   - `/setinterval 99999` → validation error (max 86400).
