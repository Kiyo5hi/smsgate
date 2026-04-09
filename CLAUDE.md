# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A LilyGo A76XX / SIM7xxx firmware that forwards incoming SMS to a Telegram bot
over WiFi + HTTPS. Forked out of the `examples/` tree of LilyGo-Modem-Series.

## Current capabilities

What works today (verified on real T-A7670X hardware):

- Receives an SMS via `+CMTI` URC, decodes UCS2 (Chinese tested), forwards
  to a single Telegram chat with sender + timestamp + body, and deletes
  the message from the SIM. End-to-end latency ~1s.
- Drains any SMS that arrived while the bridge was offline at startup.
- Reboots itself after 8 consecutive Telegram POST failures to escape
  stuck TLS / WiFi states.

What does **not** work yet:

- TLS is currently `setInsecure()` — not actually verifying the cert.
  See `rfc/0001`.
- Long / concatenated SMS arrive as un-stitched fragments because we
  use text mode, not PDU. See `rfc/0002`.
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
   `setup()` in `src/main.cpp` for `NETWORK_APN` — it's commented out
   by default. If your SIM is from a Chinese / regional operator that
   requires one (e.g. `CHN-CT` for China Telecom) you need to define
   it via `-DNETWORK_APN="..."` or uncomment the macro. Without it,
   network registration may be silently denied.
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

### SMS receive path (URC-driven, not polling)

1. `setup()` configures: `+CMGF=1` (text mode), `+CSCS="UCS2"` (everything
   comes back as UTF-16BE hex, sender included), `+CSDH=1`, and
   `+CNMI=2,1,0,0,0` so new SMS produce a `+CMTI: "SM",<idx>` URC.
2. `loop()` reads raw lines from `SerialAT`, looks for `+CMTI:`, extracts
   `<idx>`.
3. `handleSmsIndex(idx)` issues `AT+CMGR=<idx>`, parses sender/timestamp/
   content out of the response, decodes UCS2 → UTF-8, posts to Telegram,
   and `AT+CMGD`s the slot **only on success**. On failure the SMS stays
   on the SIM and a counter increments; after `MAX_CONSECUTIVE_FAILURES`
   (8) the ESP reboots.
4. `sweepExistingSms()` runs once at the end of `setup()` to drain any
   messages that arrived while the bridge was offline.

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

Currently `WiFiClientSecure` runs with `setInsecure()`. The pinned
ISRG Root X1 cert is in the source but unused — see `rfc/0001-tls-cert-pinning.md`
for the open work. Cert verification failed in testing on the network this
board is deployed on.

### Telegram POST response handling

`sendBotMessage()` keeps the connection alive (`Connection: keep-alive`) for
throughput, but **must drain the full Content-Length** of every response.
If you early-`break` after spotting `"ok":true`, leftover bytes sit in the
TLS buffer and the next request reads them as the new HTTP status line.
This was a real bug. Don't reintroduce it.

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
