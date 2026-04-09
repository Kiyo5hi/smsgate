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
- Reboots itself after 8 consecutive Telegram POST failures to escape
  stuck TLS / WiFi states.

What does **not** work yet:

- TLS is currently `setInsecure()` — not actually verifying the cert.
  See `rfc/0001`.
- One-way only. No Telegram → SMS replies. See `rfc/0003`.
- Hard dependency on WiFi. The SIM's data path is unused. See `rfc/0004`.

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
  modem power-up, WiFi, NTP, and the `+CMTI` URC drainer in `loop()`.
  Constructs a `RealModem`, `RealBotClient`, and a reboot lambda, and
  wires them into an `SmsHandler`. No SMS logic lives here.
- `src/sms_codec.{h,cpp}` — pure (Arduino-`String`-only) helpers:
  `decodeUCS2`, `parseCmgrBody`, `humanReadablePhoneNumber`,
  `timestampToRFC3339`. No hardware deps. Compiled into both the
  firmware env and the native test env.
- `src/sms_handler.{h,cpp}` — stateful SMS pipeline as a class.
  Constructor takes `IModem&`, `IBotClient&`, and a `RebootFn`. Owns
  the consecutive-failure counter and `MAX_CONSECUTIVE_FAILURES = 8`.
  Methods: `handleSmsIndex(int)`, `sweepExistingSms()`.
- `src/imodem.h` — narrow interface over TinyGSM (`sendAT`,
  `waitResponse`, `waitResponseOk`). `RealModem` in `real_modem.h` is
  the production adapter; `FakeModem` in `test/support/` is the test
  double.
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
- `test/test_native/` — Unity tests for `sms_codec` and `sms_handler`.
  Runs on the host via `pio test -e native`.
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
`build_src_filter = +<sms_codec.cpp> +<sms_handler.cpp> -<main.cpp>
-<telegram.cpp>` so hardware-dependent TUs never hit the host compiler.
See `rfc/0007-testability-native-tests-di.md` for the full design.

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
  `### Unhandled: ...` in debug mode. That includes `+CMTI`, so SMS arrival
  events would silently disappear. We drain `SerialAT` ourselves.

### TLS state

Currently `WiFiClientSecure` (in `telegram.cpp`) runs with `setInsecure()`.
The pinned ISRG Root X1 cert is in the source but unused — see
`rfc/0001-tls-cert-pinning.md` for the open work. Cert verification failed
in testing on the network this board is deployed on.

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
