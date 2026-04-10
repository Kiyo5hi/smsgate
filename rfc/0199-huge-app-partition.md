---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0199: Switch to `huge_app.csv` partition scheme (3MB app)

## Motivation

The firmware hit 89.9% of the default 1.25MB app partition (1178277 / 1310720
bytes) after RFC-0198, leaving ~1KB of headroom. This would block all further
feature development.

The ESP32 on the LilyGo T-A7670X has 4MB flash. The default partition scheme
wastes 1.25MB on a second OTA app slot (`app1`) that we never use — all firmware
updates are done via USB with `pio run -t upload`, not over-the-air.

## Plan

Set `board_build.partitions = huge_app.csv` in `[esp32dev_base]` in
`platformio.ini`. This pre-bundled scheme ships with the esp32 Arduino
framework and allocates:

```
nvs:      20KB  (unchanged — Preferences storage unaffected)
otadata:   8KB  (kept — marks app0 as valid after boot)
app0:    3MB    (up from 1.25MB — 2.3× more headroom)
spiffs: 896KB  (not used by us; retained for future FS use)
coredump: 64KB (crash dump capture)
```

## Result

Flash usage drops from 89.9% → 37.5% (1178277 / 3145728 bytes),
freeing ~1.96MB of headroom for future features.

## Notes for handover

- OTA updates are NOT supported — only USB flashing. The `otadata`
  partition is kept solely to mark `app0` as valid at boot (the bootloader
  requires it). If OTA is ever needed, switch back to `default.csv` and
  split the 3MB between two 1.5MB app slots.
- The NVS partition is unchanged, so all persisted settings survive the
  partition table change (NVS is at the same offset 0x9000).
- **WARNING**: Flashing with this partition scheme will erase the old
  `app1` and `spiffs` regions. NVS is preserved. A one-time full flash
  cycle is needed for existing devices (`pio run -t upload` is sufficient
  since it flashes the firmware and the partition table together via
  esptool).
