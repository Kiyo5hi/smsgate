---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0143: /modeminfo command — show IMEI, model, firmware revision

## Motivation

When managing multiple devices it helps to see IMEI and firmware
revision from Telegram without connecting a serial monitor.
The existing `/sim` command (RFC-0105) shows ICCID/operator/CSQ
but not IMEI or modem firmware revision.

## Plan

Add `setModemInfoFn(std::function<String()>)` setter to `TelegramPoller`.
The fn collects IMEI (`AT+GSN`), model (`AT+CGMM`), and firmware
revision (`AT+CGMR`) and returns a formatted string.
In `main.cpp` wire it via `modem.getIMEI()`, `modem.getModemName()`,
and `modem.getModemInfo()` (TinyGSM wrappers for these AT commands).

Command: `/modeminfo` → calls fn if set, else "(modem info not configured)".

## Notes for handover

TinyGSM's `getIMEI()` → AT+GSN, `getModemName()` → AT+CGMM,
`getModemInfo()` → AT+CGMR. All are synchronous and fast (<200ms).
