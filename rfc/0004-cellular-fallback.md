---
status: proposed
created: 2026-04-09
---

# RFC-0004: Use the modem's own data path as a fallback / primary

## Motivation

The bridge currently depends on WiFi to reach Telegram. That defeats one
of the main reasons to use a cellular dev board: the SIM card is the
network. If WiFi is unavailable (the user is mobile, the AP dies, the
captive portal blocks egress) the bridge is dead even though it has a
working SIM in its hand.

## Current state

`connectToWiFi()` in `setup()` is a hard requirement. There is no
GPRS/LTE data path. `WiFiClientSecure` is used unconditionally.

## Plan

1. Add `modem.gprsConnect(apn, ...)` after network registration. APN
   needs to be configurable per-SIM; make it a `secrets.h` define
   defaulting to empty.
2. Use `TinyGsmClientSecure` (TinyGSM's built-in TLS over the modem)
   instead of `WiFiClientSecure`. The TinyGSM fork in the upstream
   repo supports this for A76XX.
3. Strategy: prefer WiFi if connected, fall back to cellular on
   failure. Or invert: cellular primary, WiFi only if explicitly
   provisioned. The latter makes the bridge work out-of-the-box on a
   bare board with just a SIM.
4. Re-test the same cert pinning question (RFC-0001) over the
   cellular path — the chain might be different.

## Notes for handover

- TinyGSM's `Client` interface is duck-typed close enough to
  `WiFiClient` that `sendBotMessage()` should work mostly unchanged.
  The TLS API surface (`setCACert`, `setCACertBundle`,
  `setInsecure`) **differs** between `WiFiClientSecure` and
  `TinyGsmClientSecure` — expect to factor out a small wrapper.
- Power: cellular data is much more expensive than WiFi. If the device
  is battery-powered, gate cellular fallback behind battery level
  monitoring.
- This RFC partly subsumes the original "drop WiFi entirely" idea.
  Don't drop WiFi — just stop requiring it.
