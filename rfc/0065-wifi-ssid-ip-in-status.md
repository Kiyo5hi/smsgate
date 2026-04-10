---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0065: WiFi SSID and IP in /status

## Motivation

The `/status` WiFi line showed only RSSI (`-NNdBm`). Adding the SSID and IP
makes it easy to verify the bridge is on the right network and reachable.

## Plan

**`src/main.cpp`** `statusFn`:
- Replace `"  WiFi: " + RSSI + " dBm\n"` with:
  `"  WiFi: <SSID> (-NNdBm)  <IP>"` when connected, or `"disconnected"`.

## Notes for handover

Only `src/main.cpp` changed. No test changes needed.
