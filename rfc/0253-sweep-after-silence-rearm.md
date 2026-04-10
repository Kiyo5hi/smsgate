---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0253: Sweep SIM after modem health-check silent-reset re-arm

## Motivation

The modem health check (RFC-0234/RFC-0239) fires when there has been no
serial activity from the modem for 2+ minutes.  If `AT` responds OK, the
RFC-0242 re-arm block restores `+CMGF=0`, `+CSDH=1`, `+CNMI`, `+CLIP`,
and `+CREG=1`.

The re-arm is triggered precisely when the modem might have undergone a
**silent internal reset** — one that didn't send `RDY` / `APP READY` /
`SMS Ready` / `PB DONE` ready indicators (which would have triggered the
RFC-0245 handler with its own sweep).  A silent reset clears `+CNMI`,
so any `+CMTI` URCs for SMS that arrived during the silence window were
never generated — but the SMS are still on the SIM.

Before this RFC:
- SMS arrives → stored on SIM → modem resets silently → `+CMTI` never sent
  → health check fires 2-4 min later → re-arm restores `+CNMI`
  → next SMS finally generates `+CMTI`, but old one is stranded until the
  30-min periodic sweep (RFC-0235).

## Plan

After the 5-command re-arm sequence, add a defensive `sweepExistingSms()`
when transport is available.  `sweepExistingSms()` already has per-slot
WDT kicks (RFC-0248) and is a no-op if the SIM is empty.

```cpp
if (activeTransport != ActiveTransport::kNone)
    smsHandler.sweepExistingSms();
```

The post-re-arm `esp_task_wdt_reset()` at line 3101 follows immediately
after, so WDT coverage is maintained.

## Notes for handover

This sweep complements RFC-0245 (sweep on explicit ready indicators).
RFC-0245 handles the case where the modem announces its reset; this RFC
handles the case where it doesn't.  Both paths lead to the same outcome:
SMS stranded on the SIM are drained as soon as the device detects the
modem recovered.

The health check fires at most every `kModemCheckIntervalMs` (2 min) and
only when `modemSilent` is true.  The sweep adds at most one `AT+CMGL`
exchange (~1 s for an empty SIM, up to ~10 s × N messages otherwise,
each with a WDT kick) every 2+ minutes.
