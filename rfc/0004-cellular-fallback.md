---
status: implemented
created: 2026-04-09
updated: 2026-04-09
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

## Review

verdict: approved-with-changes

### Issues

- **BLOCKING — Wrong TinyGSM class for A7670.** The RFC says "use
  `TinyGsmClientSecure`", which implies the standard upstream TinyGSM
  SSL client. In the LilyGo fork the A7670 is defined by
  `TINY_GSM_MODEM_A7670` and is served by `TinyGsmClientA7670.h`,
  which inherits `TinyGsmTCP` but does NOT include `TinyGsmSSL.tpp`.
  The SSL variant is a separate class in `TinyGsmClientA76xxSSL.h`
  (`TinyGsmA76xxSSL` / `GsmClientSecureA76xxSSL`), which requires
  compiling with `TINY_GSM_MODEM_A76XX_SSL` instead of
  `TINY_GSM_MODEM_A7670`. The RFC must name the correct class and note
  that the build flag switches the modem type (and therefore the MUX
  count: the SSL variant is 2-mux, the plain variant is 10-mux).

- **BLOCKING — No `setCACertBundle` on the modem-side TLS client.**
  `GsmClientSecureA76xxSSL::setCertificate(name)` takes a filename
  string that refers to a certificate that has already been uploaded to
  the modem's flash via `AT+CCERTDOWN`. It does NOT accept an in-memory
  bundle pointer the way `WiFiClientSecure::setCACertBundle()` does.
  The entire RFC-0001 cert bundle mechanism is ESP-IDF-specific and
  cannot be reused verbatim. The cellular TLS path requires: (a)
  uploading the relevant root CA as a PEM file to the modem at
  first-boot or firmware-flash time via
  `TinyGsmA76xxSSL::downloadCertificate(filename, pem_string)`, (b)
  passing that filename string to `GsmClientSecureA76xxSSL::setCertificate()`
  before connecting. Without this step, `authmode` stays 0 (no
  authentication), which means TLS verification is silently skipped —
  the equivalent of `setInsecure()` but without any log warning. This
  must not be left implicit; the RFC must require either (a) explicit
  unauthenticated-cellular mode with the same `[SECURITY WARNING]` log
  line pattern from RFC-0001, or (b) a defined cert-provisioning step.

- **BLOCKING — Transport abstraction is required, not optional.**
  `keepTelegramClientAlive()` and the entire HTTP I/O in
  `telegram.cpp` is coded directly against `WiFiClientSecure&`
  (file-static, returned by value-of-reference). Swapping the
  transport at runtime requires either a `Client*` pointer swap or a
  polymorphic transport wrapper passed into `RealBotClient`. The RFC
  mentions "factor out a small wrapper" in passing, but this is the
  central mechanical problem. Conditional compilation
  (`#ifdef USE_CELLULAR`) with a parallel code path duplicating the
  full HTTP logic would be unmaintainable; the RFC should commit to a
  `Client&` or `Client*` abstraction at `RealBotClient`'s boundary
  (injected at construction from `main.cpp`) so the HTTP logic stays
  in one place. This also means `setupTelegramClient()` must be
  rethought — it currently owns the static `WiFiClientSecure` and
  returns bool; with two possible transports it needs to accept or
  return the active transport object.

- **BLOCKING — `TINY_GSM_MODEM_A76XX_SSL` conflicts with PDU-mode SMS
  path.** The bridge currently uses `TinyGsm modem(SerialAT)` with
  `TINY_GSM_MODEM_A7670`. The SMS receive and PDU-send paths (RFC-0002,
  RFC-0003) depend on `sendAT` / `waitResponse` methods that are
  present on both the plain and SSL variants, so those remain intact.
  However, switching to `TinyGsmA76xxSSL` also drops the MUX count
  from 10 to 2. If any future code allocates more than two concurrent
  sockets this will silently break. The RFC should call this out and
  confirm 1 SSL socket + 0 spare is acceptable for the current use
  case.

- **NON-BLOCKING — Strategy choice: WiFi-primary is correct for this
  deployment.** The board is USB-powered and at a fixed location with
  WiFi. WiFi-primary / cellular-fallback is the right default: WiFi
  has lower latency, no data cost, and no APN complexity. The RFC's
  suggestion to invert to "cellular primary, WiFi optional" would add
  APN provisioning friction and modem-data cost for every message on a
  device that already has a working AP. Keep WiFi primary; cellular
  fallback is the value add. The power-draw concern is a red herring
  for a USB-powered device — strike the battery-level caveat or scope
  it explicitly to a "(battery deployment only)" note.

- **NON-BLOCKING — APN in `secrets.h` is sufficient for a single-SIM
  deployment.** Adding `#define NETWORK_APN ""` with a comment in
  `secrets.h.example` (defaulting to empty, meaning no explicit APN
  needed for many SIMs) is the right pattern and matches what the
  existing code already does with `NETWORK_APN` in `main.cpp`. No
  runtime detection is needed. One clarification to add: the bridge
  should attempt `gprsConnect` only if `NETWORK_APN` is defined and
  non-empty, so users without a required APN can opt out without
  editing code.

- **NON-BLOCKING — Cellular TLS chain may differ from WiFi.**
  When a cellular carrier inspects or proxies HTTPS traffic, the
  intermediate chain served to the modem can differ from what the same
  host presents over a direct internet path. The existing cert bundle
  (ISRG Root X1/X2, DigiCert, Go Daddy G2 + Class 2 cross-sign) was
  validated only over WiFi. Before shipping the cellular path, run
  `openssl s_client -connect api.telegram.org:443` through the modem's
  data bearer (e.g. using `AT+CCHOPEN` in a test sketch) to confirm
  the chain. If the carrier terminates TLS and re-signs with their own
  root, the bundle is irrelevant and the `setInsecure` escape hatch
  (with its required warning log line) is the only practical option.

- **NON-BLOCKING — Scope is reasonable; no need to split further.**
  "WiFi primary + cellular fallback" is a single coherent feature. The
  concern about doing too much in one shot does not apply here: the
  mechanical work is bounded (transport abstraction + GPRS connect +
  cert provisioning step), and splitting "add cellular" from "add
  fallback logic" would leave a half-working state on the main branch.
  Implement the full fallback in one PR.

### Summary

The plan is directionally correct but has three hard blockers before any
code is written. First, the RFC must name `TinyGsmA76xxSSL` /
`GsmClientSecureA76xxSSL` (not the generic `TinyGsmClientSecure`) and
acknowledge the `TINY_GSM_MODEM_A76XX_SSL` build-flag switch and
reduced MUX count. Second, the TLS cert mechanism is fundamentally
different on the modem side — `setCACertBundle()` does not exist; certs
must be uploaded to modem flash via `AT+CCERTDOWN` before first use,
and the RFC needs a defined provisioning step or an explicit
unauthenticated-mode policy. Third, `RealBotClient` must receive the
active transport via dependency injection (a `Client&` or `Client*` at
construction time) rather than hardcoding the `WiFiClientSecure`
file-static — otherwise the fallback logic either duplicates the full
HTTP layer or requires unsafe pointer swaps at runtime. Fix those three,
keep WiFi as primary, and this is ready to implement.

## Code Review

verdict: approved-with-changes

### Issues

- **BLOCKING — Static initialization order: `telegramModemClient(modem, 0)`.**
  `telegram.cpp` line 50 initializes `static TinyGsmClientSecure
  telegramModemClient(modem, 0)` at file scope, where `modem` is an
  `extern TinyGsm` defined at file scope in `main.cpp`. C++ gives no
  guarantee on the relative initialization order of file-scope statics
  across translation units. If `telegramModemClient` is constructed
  before `modem`, the `GsmClientSecureA76xxSSL` constructor calls
  `stop()` on a not-yet-constructed `TinyGsmA76xxSSL`, which writes to
  `sockets[0]` via `at->sockets[this->mux] = this` — undefined
  behaviour. In practice the linker usually puts `main.cpp` objects
  first because they appear first in the build, but this is not
  guaranteed and will silently break on any relink that reverses the
  order. Fix: move `telegramModemClient` construction into
  `setupCellularClient()` as a `static` local (guaranteed to be
  initialized on first call, after `modem` is definitely live) or pass
  the modem reference into `setupCellularClient()` as a parameter and
  heap-allocate the client there. The `extern TinyGsm modem` pattern is
  acceptable for calling methods but must not be used as a constructor
  argument at file scope.

- **BLOCKING — `connectToWiFi()` called from `loop()` blocks for up to
  15 s, eating URCs.** When `activeTransport == kCellular`, the
  transport-check block in `loop()` (main.cpp ~line 642) calls
  `connectToWiFi()`, which spins in a `for (int i = 0; i < 30; ...)
  delay(500)` loop for up to 15 s waiting for `WL_CONNECTED`. During
  that window `SerialAT` is not drained. An SMS arriving during a
  WiFi-recovery attempt will fill the UART FIFO; if more than ~128
  bytes arrive the modem's `+CMTI` URC is silently dropped and the SMS
  sits unretrieved on the SIM until the next `sweepExistingSms` at
  reboot. The same hazard applies to `syncTime()`, which can also block
  indefinitely (no timeout). For the loop() call site, replace
  `connectToWiFi()` with a non-blocking check: call
  `WiFi.begin(ssid, password)` once and return immediately; on the
  *next* 30-second tick check `WiFi.status() == WL_CONNECTED` and only
  then call `setupTelegramClient`. The `setup()` call site (one-time,
  before any URCs can arrive) is fine as-is.

- **NON-BLOCKING — `[CELLULAR TLS]` warning printed on every reconnect,
  not just at setup.** `keepTransportAlive()` calls `transport->connect()`
  on every dropped connection; `setupCellularClient()` is also called
  from the loop() fallover path. The `[CELLULAR TLS]` warning is only
  inside `setupCellularClient()` (telegram.cpp line 111), so it is not
  repeated on each reconnect — this is correct. However,
  `setupCellularClient()` itself is also called from the loop() WiFi-drop
  handler (main.cpp ~line 620 and ~line 668), meaning the warning fires
  every time the cellular path is re-entered. This is intentional and
  acceptable (any reconnect after a gap re-establishes the insecure
  path), but could spam the log if `setupCellularClient` is called
  repeatedly in a tight retry loop. Not a crash risk; worth noting.

- **NON-BLOCKING — `wifiDownLastCheck` is not reset on successful
  cellular setup.** In the WiFi-drop handler (main.cpp ~lines 616-626),
  when `setupCellularClient` succeeds, `wifiDownLastCheck` is set to
  `false`. But if `gprsConnect` succeeds while `setupCellularClient`
  fails, `wifiDownLastCheck` remains `true`. On the next 30-second tick
  the code will attempt `gprsConnect` again (because `activeTransport`
  is still `kWiFi` and `wifiDownLastCheck` is still `true`) — which is
  the desired retry behaviour, so this is correct. The comment on line
  623 (`wifiDownLastCheck = false`) implies it should always be reset,
  but omitting the reset in the failure path is actually the right
  choice. Consider adding a comment to make this explicit.

- **NON-BLOCKING — `CELLULAR_APN` defined as `""` by default in
  `secrets.h.example`, which means cellular is always compiled in for
  users who copy the example verbatim.** The `#if defined(CELLULAR_APN)
  && defined(TINY_GSM_MODEM_A76XXSSL)` guard on main.cpp line 353
  evaluates true when `CELLULAR_APN` is defined to the empty string
  `""`. The resulting behaviour (trying `gprsConnect("")` on every
  setup failure) is benign — the connect call will fail or succeed
  depending on the SIM — and the `[CELLULAR TLS]` warning documents the
  security trade-off. But a user who has no SIM or does not want
  cellular fallback would need to actively comment out the `#define` to
  avoid spurious `gprsConnect` attempts. Consider adding a note in
  `secrets.h.example` that the define should be removed (not just
  emptied) to compile out cellular entirely.

- **NON-BLOCKING — `sendPduSms` and all SMS/call methods survive the
  class switch.** Confirmed by inspection: `TinyGsmClientA76xxSSL` (the
  2-MUX SSL class) inherits from `TinyGsmA76xx<TinyGsmA76xxSSL>`, which
  includes `TinyGsmSMS.tpp` and `TinyGsmCalling.tpp`. `sendSMS`,
  `sendSMS_UTF16`, `callHangup`, `getRegistrationStatus`,
  `getSignalQuality`, `testAT`, `sendAT`, and `waitResponse` are all
  present on the SSL class. `sendPduSms` in `RealModem` writes directly
  to `SerialAT` after an `AT+CMGS` prompt — it does not use the MUX
  socket layer at all — so the MUX count reduction from 10 to 2 does
  not affect it. No regression here.

- **NON-BLOCKING — 2-MUX limit is safe for current use.** The bridge
  uses exactly one SSL socket (MUX 0, `telegramModemClient`) for
  Telegram HTTPS. AT command traffic (SMS, calls, registration) goes
  directly over the stream without occupying a MUX slot. 1 of 2 MUX
  slots used; 1 spare. Adequate for the current use case.

### Summary

Two blockers need fixing before this lands. The static initialization
order of `telegramModemClient(modem, 0)` at file scope in `telegram.cpp`
is undefined behaviour when the TU link order is not guaranteed — move
it to a static local inside `setupCellularClient()`. The `connectToWiFi()`
call inside the loop() transport-check block blocks for up to 15 s,
silently dropping URCs during that window — replace it with a
non-blocking WiFi status poll across two 30-second ticks. Everything
else is sound: the `TINY_GSM_MODEM_A76XXSSL` class switch is correct
and preserves all SMS/call methods, the `Client*` transport injection
via `setTransport()` is the right abstraction, the `[CELLULAR TLS]`
warning is appropriately scoped to `setupCellularClient()`, and the
2-MUX limit is safe for single-socket use.
