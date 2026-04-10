---
status: in-progress
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0248: WDT kick between each handleSmsIndex call in sweepExistingSms

## Motivation

`sweepExistingSms()` issues `AT+CMGL="ALL"`, collects all SIM slots, then
calls `handleSmsIndex(idx)` in a tight loop — each call takes ~10 s
(AT+CMGR + Telegram HTTPS roundtrip).  With 13 or more messages on the
SIM, the sweep exceeds the 120 s watchdog window between the pre- and
post-sweep `esp_task_wdt_reset()` calls in `main.cpp`, causing a spurious
reboot mid-drain.

## Plan

In `sms_handler.cpp`, add `#ifdef ESP_PLATFORM` / `esp_task_wdt_reset()`
immediately before each `handleSmsIndex(idx)` call inside the sweep loop.
Follow the existing pattern in `sms_sender.cpp` (RFC-0015) and
`telegram.cpp` (RFC-0028): `#ifdef ESP_PLATFORM` include at the top of the
file and `#ifdef ESP_PLATFORM esp_task_wdt_reset(); #endif` at each kick
site.

This keeps the WDT kick out of the host/native test path without any
interface change to `SmsHandler`.

## Notes for handover

`sweepExistingSms()` is called from 7–8 places in `main.cpp`, all already
surrounded by `esp_task_wdt_reset()` pairs (pre- and post-sweep kicks from
RFC-0241/RFC-0243).  Those outer kicks remain.  The new inner kick runs
before each individual `handleSmsIndex` call, so the worst-case WDT
exposure is now bounded by a single `handleSmsIndex` call (~10 s, well
under 120 s) rather than the full batch.
