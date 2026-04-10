---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0149: /health command — compact single-line health check

## Motivation

`/status` is verbose (10+ lines). During incident triage the operator
needs a quick single-line "is it alive?" signal. `/ping` only confirms
Telegram connectivity; `/health` adds WiFi/cell/heap at a glance.

## Plan

Add `setHealthFn(std::function<String()>)` setter to `TelegramPoller`.
Production wires a lambda returning e.g.:
  "✅ OK | WiFi: -65dBm | CSQ: 18 | Heap: 48KB | Up: 2d3h"
or "⚠️ WiFi down | CSQ: 14 | Heap: 50KB | Up: 1h"

Command: `/health` → calls fn if set, else "(health not configured)".
