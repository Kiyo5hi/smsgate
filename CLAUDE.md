# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A LilyGo A76XX / SIM7xxx firmware that forwards incoming SMS to a Telegram bot
over WiFi + HTTPS. Forked out of the `examples/` tree of LilyGo-Modem-Series.

## Current capabilities

What works today (verified on real T-A7670X hardware):

- Receives an SMS via `+CMTI` URC, decodes the PDU (GSM-7 default
  alphabet, UCS-2, and 8-bit), forwards to a single Telegram chat with
  sender + timestamp + body, and deletes the message from the SIM.
  End-to-end latency ~1s.
- Stitches concatenated (long) SMS back together via UDH reference
  (IEI 0x00 and 0x08), buffering fragments in RAM until all parts
  arrive. Incomplete groups stay on the SIM so a reboot rehydrates them.
- Drains any SMS that arrived while the bridge was offline at startup.
- Detects incoming voice calls via `RING` + `+CLIP` URCs, posts a
  Telegram notification with the caller's number (or "Unknown" for
  withheld / anonymous callers), and auto-hangs-up the call so the
  caller gets a busy signal instead of ringing forever. See
  "Call notify" under Architecture.
- Reboots itself after 8 consecutive Telegram POST failures to escape
  stuck TLS / WiFi states.

What does **not** work yet:

- One-way only. No Telegram → SMS replies. See `rfc/0003`.
- Hard dependency on WiFi. The SIM's data path is unused. See `rfc/0004`.

TLS verification against `api.telegram.org` is now active — see the
"TLS state" subsection below. RFC-0001 is implemented.

## First-time setup

1. **Clone next to the upstream lib repo.** This project is a sibling
   of `LilyGo-Modem-Series` (see "Repo dependency" below). If you
   clone it standalone, the build will fail with a `lib_extra_dirs`
   path error pointing at `../LilyGo-Modem-Series/lib`.
2. **Get a Telegram bot token and chat id.** See the comments in
   `src/secrets.h.example` — short version: talk to `@BotFather` to
   create the bot, talk to `@userinfobot` to get your numeric chat id.
3. **Create `src/secrets.h`** by copying `secrets.h.example` and filling
   in WiFi + bot credentials. The file is gitignored.
4. **Check whether your operator needs an APN.** Look near the top of
   `src/main.cpp` for `NETWORK_APN` — it's commented out by default.
   If your SIM is from a Chinese / regional operator that requires one
   (e.g. `CHN-CT` for China Telecom) you need to define it via
   `-DNETWORK_APN="..."` or uncomment the macro. Without it, network
   registration may be silently denied.
5. **Build, flash, watch the monitor.** A healthy boot sequence looks
   roughly like:
   ```
   Probing modem... → Modem silent, pulsing PWRKEY... (or "already powered on")
   Model Name:A7670G-... → Wait SMS Done. → Network registration successful
   Connecting to WiFi → WiFi connected!
   Syncing time... → Current time: ...
   Reconnected to Telegram API server! → Telegram status: HTTP/1.1 200 OK
   ```
   If the log freezes at `Probing modem...` for more than ~10s,
   power-cycle the whole board (USB unplug) — the modem is in a state
   we can't recover via PWRKEY alone.

## Repo dependency

This project does **not** vendor the TinyGSM fork or board pin definitions.
`platformio.ini` references the upstream repo via relative paths:

```
lib_extra_dirs = ../LilyGo-Modem-Series/lib
boards_dir     = ../LilyGo-Modem-Series/boards
```

So `LilyGo-Modem-Series` must exist as a sibling directory at
`../LilyGo-Modem-Series` (i.e. `<parent>/LilyGo-Modem-Series` and
`<parent>/lilygo-sms-telegram-bridge`). If you move this repo, update
those two paths.

## Build / flash / monitor

PlatformIO is **not** on the bash `PATH` on this machine. Use the explicit
penv binary:

```bash
PIO="/c/Users/Kiyoshi Guo/.platformio/penv/Scripts/pio.exe"

# Build (default env is T-A7670X)
"$PIO" run -e T-A7670X

# Flash
"$PIO" run -e T-A7670X -t upload --upload-port COM3

# Find the board
"$PIO" device list
```

The board is a LilyGo T-A7670X, USB serial chip CH9102, normally enumerated
as **COM3** on this machine.

### Monitor gotcha

`platformio.ini` enables the `esp32_exception_decoder` filter. That filter
swallows raw output and only emits `"Please build project in debug
configuration..."` when run inside the Claude harness. **Always** monitor
with `--raw` so you actually see boot logs:

```bash
"$PIO" device monitor --port COM3 -b 115200 --raw --quiet
```

For automated capture from a non-interactive shell, prefer pyserial
directly (no DTR/RTS toggling required after a fresh `upload` — esptool
already reset the board). The `pio.exe` python is at
`~/.platformio/penv/Scripts/python.exe` and ships with `pyserial`.

## Architecture

### Source layout

- `src/main.cpp` — composition root. Owns the `TinyGsm modem` instance,
  modem power-up, WiFi, NTP, and the `+CMTI` / `RING` / `+CLIP` URC
  drainer in `loop()`. Constructs a `RealModem`, `RealBotClient`, and
  a reboot lambda, and wires them into an `SmsHandler` + a
  `CallHandler`. No SMS / call logic lives here.
- `src/sms_codec.{h,cpp}` — pure (Arduino-`String`-only) helpers:
  `decodeUCS2`, `parseCmgrBody`, `humanReadablePhoneNumber`,
  `timestampToRFC3339`, `parseClipLine`. No hardware deps. Compiled
  into both the firmware env and the native test env.
- `src/sms_handler.{h,cpp}` — stateful SMS pipeline as a class.
  Constructor takes `IModem&`, `IBotClient&`, and a `RebootFn`. Owns
  the consecutive-failure counter and `MAX_CONSECUTIVE_FAILURES = 8`.
  Methods: `handleSmsIndex(int)`, `sweepExistingSms()`.
- `src/call_handler.{h,cpp}` — stateful incoming-call pipeline as a
  class. Constructor takes `IModem&`, `IBotClient&`, and a `ClockFn`.
  Consumes `RING` / `+CLIP` URC lines via `onUrcLine(line)`; the
  loop also calls `tick()` each iteration to drive the unknown-number
  deadline and the cooldown-to-idle transition.
- `src/imodem.h` — narrow interface over TinyGSM (`sendAT`,
  `waitResponse`, `waitResponseOk`, `callHangup`). `RealModem` in
  `real_modem.h` is the production adapter; `FakeModem` in
  `test/support/` is the test double.
- `src/ibot_client.h` — narrow interface for "post text to the bot".
  `RealBotClient` in `telegram.cpp` is the production impl; owns the
  file-static `WiFiClientSecure` and the full Content-Length drain
  loop. `FakeBotClient` in `test/support/` is the test double.
- `src/real_modem.h` — header-only `RealModem` class, thin delegate to
  `TinyGsm`. Only included from `main.cpp`.
- `src/telegram.{h,cpp}` — `setupTelegramClient()` + `RealBotClient`.
  Still has the ISRG Root X1 cert constant staged for RFC-0001.
- `src/utilities.h` — board pin definitions, copied verbatim from
  upstream `LilyGo-Modem-Series/examples/*/utilities.h`. Selects the
  right `TINY_GSM_MODEM_xxx` based on the `LILYGO_T_*` build flag.
- `src/secrets.h` — gitignored, see "Secrets" below.
- `test/test_native/` — Unity tests for `sms_codec`, `sms_handler`,
  and `call_handler`. Runs on the host via `pio test -e native`.
- `test/support/` — hand-rolled `Arduino.h` stub (+ `.cpp` storage for
  the `Serial` no-op), plus `fake_modem.h` and `fake_bot_client.h`.
  The stub covers ~18 `String` methods actually used by the modules
  under test; see the big comment at the top of `Arduino.h` for the
  exact subset.

### Running host unit tests

The firmware logic is exercised on the host via a PlatformIO `native`
env and Unity. No hardware needed:

```bash
# Run all host tests
"$PIO" test -e native

# Explicit filter (equivalent here — only one test folder)
"$PIO" test -e native -f test_native
```

The native env needs a host C/C++ compiler in `PATH`. On this machine
MinGW-w64 is installed via winget as
`BrechtSanders.WinLibs.POSIX.UCRT`, at
`C:\Users\Kiyoshi Guo\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin`.
Prepend that directory to `PATH` before running `pio test`:

```bash
export PATH="/c/Users/Kiyoshi Guo/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH"
```

The native env (`[env:native]` in `platformio.ini`) uses
`build_src_filter = +<sms_codec.cpp> +<sms_handler.cpp>
+<call_handler.cpp> -<main.cpp> -<telegram.cpp>` so hardware-dependent
TUs never hit the host compiler. See
`rfc/0007-testability-native-tests-di.md` for the full design.

### SMS receive path (URC-driven, not polling)

1. `setup()` (main.cpp) configures: `+CMGF=0` (**PDU mode**, RFC-0002),
   `+CSDH=1`, and `+CNMI=2,1,0,0,0` so new SMS produce a
   `+CMTI: "SM",<idx>` URC. CSCS is intentionally NOT set — it's
   irrelevant in PDU mode, and TinyGSM's send paths flip the module
   back into text mode with their own CSCS on every `sendSMS*` call.
2. `loop()` (main.cpp) reads raw lines from `SerialAT`, looks for
   `+CMTI:`, extracts `<idx>`, and dispatches to `SmsHandler::handleSmsIndex`.
3. `handleSmsIndex(idx)` (sms_handler.cpp) issues `AT+CMGR=<idx>`,
   extracts the hex PDU from the response envelope, and hands it to
   `sms_codec::parseSmsPdu`. The parser handles GSM 7-bit (packed
   septets + extension escape), UCS-2 / UTF-16BE (with surrogate
   pairs), and 8-bit data; and when TP-UDHI is set it extracts the
   concat reference / total / part number from IEI 0x00 (8-bit ref)
   and IEI 0x08 (16-bit ref) UDH elements.
4. **Single-part** messages: post to Telegram, delete the slot on
   success, leave it on failure, bump the consecutive-failure counter
   on failure. After `MAX_CONSECUTIVE_FAILURES` (8) the ESP reboots.
5. **Concat fragments**: buffered in `SmsHandler::concatGroups_`, keyed
   by (sender, ref_number). Once all parts have arrived the assembled
   body is posted and every contributing SIM slot is `AT+CMGD`d on
   success. Incomplete groups are NOT deleted from the SIM — the SIM
   is the source of truth so a reboot rehydrates them via
   `sweepExistingSms`. Per-key hard caps (RFC-0002):
   **24-hour TTL**, **max 8 concurrent keys** (LRU evict on overflow),
   **max 2 KB per key**, **max 8 KB total across all keys** (LRU
   evict on overflow). Tests wire a mock clock via the `ClockFn`
   parameter so TTL/LRU paths are deterministic on host.
6. `sweepExistingSms()` runs once at the end of `setup()` to drain any
   messages that arrived while the bridge was offline. In PDU mode it
   reuses the same `handleSmsIndex` path, so concat rehydration works
   symmetrically at boot.

### Call notify (RFC-0005)

1. `setup()` enables `+CLIP=1` right after `+CNMI`, so each incoming
   call produces a `RING` URC followed by (or preceded by, depending
   on firmware) `+CLIP: "<number>",<type>,...` carrying the caller
   number.
2. The same `loop()` drain that dispatches `+CMTI` hands every URC
   line to `callHandler.onUrcLine(line)` after the SMS check.
   `CallHandler` only acts on lines starting with `RING` or `+CLIP:`
   and ignores everything else.
3. Order-independent state machine: seeing EITHER a `RING` or a
   `+CLIP:` transitions Idle → Ringing and starts an unknown-number
   deadline timer (`kUnknownNumberDeadlineMs = 1500ms`). Receiving
   the other half of the pair commits the event immediately.
   If only `RING` arrives and no `+CLIP` shows up before the
   deadline, the event is still committed with "Unknown" as the
   caller — driven by `callHandler.tick()` from the main loop.
4. On commit: the handler posts `📞 Incoming call from <number>
   (auto-rejected)` via `bot_.sendMessage(...)`, then calls
   `modem_.callHangup()` (which sends `ATH` on A76xx). If the TinyGSM
   call returns false, a raw `AT+CHUP` fallback is sent.
5. After commit the handler enters a Cooldown state for
   `kDedupeWindowMs = 6000ms` during which further RING / +CLIP URCs
   are silently dropped. This is what turns the ~3s repeating RING
   stream of a single call into a single Telegram notification. The
   cooldown-to-idle transition is also driven by `tick()`; two calls
   separated by more than 6s produce two notifications as expected.
6. Dedupe is keyed on TIME, not phone number, so two back-to-back
   calls from the same caller still produce two notifications once
   the cooldown expires.

### Two non-obvious traps (already fixed, do not regress)

- **The modem and the ESP32 are on independent power rails.** After an
  ESP-only reset (upload, watchdog, `ESP.restart()`) the modem may still
  be powered on from the previous session. Pulsing `BOARD_PWRKEY_PIN`
  unconditionally would **turn the modem off**, after which `modem.testAT()`
  spins forever and you see no log past `Probing modem...`. Always probe
  AT first; only pulse PWRKEY if the modem is genuinely silent.
  See the `Probing modem...` block in `setup()`.
- **Do not call `modem.maintain()` in `loop()`.** TinyGSM's `maintain()`
  internally calls `waitResponse()` which eats unknown URCs and only prints
  `### Unhandled: ...` in debug mode. That includes `+CMTI`, `RING`, and
  `+CLIP`, so SMS / call arrival events would silently disappear. We
  drain `SerialAT` ourselves.

### TLS state

TLS verification is **active**. `setupTelegramClient()` calls
`telegramClient.setCACertBundle(rootca_crt_bundle_start)` where the
symbol points at `data/cert/x509_crt_bundle.bin`, embedded into flash
via `board_build.embed_files` in `[esp32dev_base]`. The bundle is a
narrow set of roots (Go Daddy G2 + Class 2 cross-sign, DigiCert Global
Root CA + G2, ISRG Root X1 + X2) — see `data/cert/README.md` for the
selection rationale and regeneration steps. RFC-0001 is implemented.

**Escape hatch.** A `-DALLOW_INSECURE_TLS` build flag is wired in
`telegram.cpp`. When set, `setupTelegramClient()` calls `setInsecure()`
and prints `[SECURITY WARNING] TLS verification disabled via
-DALLOW_INSECURE_TLS` at boot. Default builds do **not** set this
flag; it exists only as an explicit, logged-at-boot opt-out for
networks that MITM HTTPS.

**Gotcha for future bundle edits.** The chain Telegram actually serves
terminates at the (Mozilla-removed) Go Daddy Class 2 root via a
cross-sign over Go Daddy Root G2. `esp_crt_bundle` refuses to accept
the handshake unless the bundle can look up the last self-signed
cert's own issuer — meaning Class 2 must stay in the bundle as long as
Telegram serves the cross-signed chain. See `data/cert/README.md`
"The Class 2 gotcha" for the full explanation; don't remove Class 2
without re-running `openssl s_client` against `api.telegram.org` and
confirming the chain has shortened.

### Telegram POST response handling

`sendBotMessage()` (telegram.cpp) keeps the connection alive
(`Connection: keep-alive`) for throughput, but **must drain the full
Content-Length** of every response. If you early-`break` after spotting
`"ok":true`, leftover bytes sit in the TLS buffer and the next request
reads them as the new HTTP status line. This was a real bug. Don't
reintroduce it.

### Secrets

`src/secrets.h` (gitignored) holds `WIFI_SSID`, `WIFI_PASSWORD`,
`TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`. The shape lives in
`src/secrets.h.example`.

## RFCs / handover convention

Open design decisions and pending work live under `rfc/` so other agents
can pick them up cleanly. Convention:

- Filename: `rfc/NNNN-short-slug.md`
- Frontmatter: `status` (`proposed` | `in-progress` | `accepted` |
  `implemented` | `deferred` | `rejected`), `created` (ISO date),
  `updated` (ISO date when status moves)
- Sections: **Motivation**, **Current state**, **Plan**, **Notes for handover**

When you finish a piece of work, update the RFC's frontmatter to
`implemented` rather than deleting the file — keeps the design history.
When you start work, flip to `in-progress` and put your handle / model
in the frontmatter. When you discover new open work mid-task, write a
new RFC instead of leaving a TODO comment buried in code.
