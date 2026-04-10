---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0245: Re-arm modem settings on soft-reset ready indicators

## Motivation

The A76xx modem can perform an internal soft reset during runtime (firmware
watchdog, network stack recovery, temperature protection).  When it does,
all AT settings configured in `setup()` — `+CMGF=0`, `+CSDH=1`, `+CNMI`,
`+CLIP` — revert to factory defaults.  The modem then emits one or more
ready indicators (`RDY`, `APP READY`, `SMS Ready`, `PB DONE`) that arrive
in the URC drain loop.

Before this fix, those indicators were silently ignored.  `+CNMI` returning
to default (typically `0,0,0,0,0`) means the modem stops sending `+CMTI`
URCs for new SMS.  New SMS are stored on the SIM but never forwarded until
the next RFC-0242 periodic re-arm (up to 30 minutes) or the next reboot.

## Plan

In the URC drain loop, check each line against known A76xx ready indicators:
`"RDY"`, `"APP READY"`, `"SMS Ready"`, `"PB DONE"`.  On match:

1. Re-send `+CMGF=0`, `+CSDH=1`, `+CNMI=2,1,0,0,0`, `+CLIP=1` immediately.
2. If transport is available, call `sweepExistingSms()` with WDT kicks —
   the reset window may have allowed SMS to arrive while `+CNMI` was inactive.
3. Log the event for post-mortem via `/debug`.

## Notes for handover

The AT commands issued during the drain loop are safe: they run synchronously
on the same serial port, and the drain loop resumes after they complete.  The
`waitResponseOk(2000UL)` calls are fast (<50 ms on a responsive modem).  The
sweep is gated on `activeTransport != kNone` so no failure-counter churn
happens when there is no connectivity.

The `+CMGF=0` and `+CSDH=1` restores are included because a modem soft-reset
would also revert those settings, breaking the PDU mode receive pipeline.
