---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0048: Build timestamp in `/status`

## Motivation

When debugging unexpected behaviour it's useful to confirm which firmware
version is running without requiring a serial connection. The compile-time
`__DATE__` and `__TIME__` macros are essentially free.

## Plan

**`src/main.cpp`** — in statusFn Config section:
```cpp
msg += "  Build: " + String(__DATE__) + " " + String(__TIME__) + "\n";
```

Result: `Build: Apr 10 2026 15:43:21`

## Notes for handover

One-line change to `src/main.cpp`. No test changes needed.
