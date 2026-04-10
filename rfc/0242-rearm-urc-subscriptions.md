---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0242: Periodic re-arm of +CNMI / +CLIP URC subscriptions

## Motivation

Some A76xx firmware revisions silently lose their `+CNMI` (new-message
notification) and `+CLIP` (caller-ID presentation) settings after
extended operation, after network re-registration events, or after
internal modem state changes that do not trigger a visible reset.  When
`+CNMI` is lost, the modem stops sending `+CMTI` URCs for new SMS;
the bridge stops forwarding them until the next reboot (or until RFC-0235's
30-min periodic sweep catches them by polling the SIM directly).

`setup()` writes `+CNMI=2,1,0,0,0` once. It is never refreshed unless
the device reboots.

## Plan

Re-issue `+CNMI` and `+CLIP` at two opportunistic points:

1. **Periodic sweep block (RFC-0235)** — before each 30-min
   `sweepExistingSms()` call.  Re-arming here is cheap (2 AT commands,
   < 1 s total) and ensures the subscription is fresh for the next
   30-minute window.  If any SMS arrives via `+CMTI` right after
   re-arming, the sweep that immediately follows will catch it anyway.

2. **Modem health-check success path (RFC-0234/0239)** — when the
   health-check AT probe succeeds after a silence period.  Extended
   modem silence is the scenario most likely to follow an internal modem
   state change, so re-arming at this point is targeted.

Both points use `realModem.sendAT()` + `waitResponseOk(2000UL)` (no
piggybacked-URC capture needed — the sweep / next drain loop covers any
missed `+CMTI` in those 2 s windows).

## Notes for handover

`+CNMI=2,1,0,0,0` is the non-delivery-report variant; builds with
`-DENABLE_DELIVERY_REPORTS` use `+CNMI=2,1,0,1,0` at boot.  The
re-arm uses the base variant for simplicity — delivery-report builds are
rare, and the subscription that matters most (bit 2: store in memory and
notify) is the same in both variants.
