---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0138: /imei command — show IMEI and modem model

## Motivation

When diagnosing hardware issues or managing multiple devices, knowing the
IMEI and modem firmware from Telegram avoids having to attach a serial
monitor. Currently `/status` does not include IMEI.

## Plan

Add a `setModemInfoFn(std::function<String()>)` setter to `TelegramPoller`.
The returned String contains IMEI and model/revision on separate lines.
`main.cpp` wires it via `AT+GSN` (IMEI) and `modem.getModemInfo()`.

Command: `/imei` → calls `modemInfoFn_()` if set, else "(modem info not configured)".

## Notes for handover

AT+GSN is standard; TinyGSM exposes `modem.getIMEI()` which issues AT+GSN.
`modem.getModemInfo()` issues AT+CGMR (firmware revision).
