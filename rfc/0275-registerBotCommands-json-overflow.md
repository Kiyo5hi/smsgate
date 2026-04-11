---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0275: DynamicJsonDocument overflow in registerBotCommands

## Motivation

`registerBotCommands()` in `telegram.cpp` builds a JSON array of ~97
command objects to register with Telegram's `/setMyCommands` endpoint.
The document was sized at `DynamicJsonDocument(1024)`, which overflows
after approximately 21 objects: ArduinoJson v6 needs ~40 bytes per
array element (object node + 2 member slots on 32-bit), so 97 × 40 +
overhead ≈ 3880 bytes total.

ArduinoJson silently drops insertions when the pre-allocated pool is
exhausted, so `createNestedObject()` returns a null object and all
subsequent key/value assignments on it are no-ops. The serialized
payload only contained the first ~21 commands; the other ~76 commands
never appeared in Telegram's autocomplete menu.

## Fix

Increase `DynamicJsonDocument doc(1024)` to `doc(6144)` — enough for
97 objects with room for future additions. This is a one-time heap
allocation at bot-command registration time (boot / reconnect), so the
6 KB cost is transient and well within the ESP32's 320 KB SRAM.

## Notes

- No logic change; only the document capacity is increased.
- Telegram's `/setMyCommands` cap is 100 commands per scope, so 97
  still fits.
- If the command count ever exceeds ~150, revisit the size again.
