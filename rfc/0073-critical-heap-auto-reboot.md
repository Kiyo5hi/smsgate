---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0073: Critical-heap auto-reboot

## Motivation

RFC-0066 added a low-heap warning at <15 KB but took no action beyond
notifying the user. If heap continues to leak below 8 KB the ESP32
crashes (stack overflow, malloc failure) with no Telegram notification —
the device just goes silent. A proactive reboot at a critical threshold
gives the operator a heads-up and recovers the device cleanly.

## Plan

Add a second tier to the 30-second heap check in `loop()`:

```
kHeapWarnThreshold    = 15 KB   (existing — warn, hysteresis at 25 KB)
kHeapCriticalThreshold =  8 KB   (new — send final message and reboot)
```

**`src/main.cpp`**:
- Add `static constexpr uint32_t kHeapCriticalThreshold = 8u * 1024u;`
  near the other heap constants (or inline in the check).
- In the 30-second heap check, BEFORE the existing <15 KB branch:
  ```cpp
  if (freeHeap < kHeapCriticalThreshold) {
      realBot.sendMessage("💀 Critical heap: N B — rebooting now.");
      delay(500);
      ESP.restart();
  }
  ```
  No hysteresis flag needed — we reboot immediately.

## Notes for handover

Changed: `src/main.cpp`, `rfc/0073-critical-heap-auto-reboot.md`.

No new tests — the 30-second periodic block is not covered by the
native test suite (it requires an ESP32 runtime).
