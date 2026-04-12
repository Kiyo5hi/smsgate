# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 firmware in Rust — bridges SMS and IM (Telegram and others); two-way forwarding.
Hardware: LilyGo T-A7670X (ESP32 + A7670G modem, CH9102 USB bridge).
This branch (`main`) is hardware-tested and boots to working state on real hardware.

## Commands

```bash
# Host tests — no hardware needed; use after every change
cargo test --no-default-features --features testing

# Single test file
cargo test --no-default-features --features testing --test <name>

# Build firmware (requires Xtensa toolchain — see Toolchain Setup below)
# Windows: must set CARGO_TARGET_DIR to a short path due to ESP-IDF path length limits
CARGO_TARGET_DIR=/c/t cargo +esp build --release --target xtensa-esp32-espidf

# Flash + monitor (Windows COM port, e.g. COM3)
espflash flash /c/t/xtensa-esp32-espidf/release/smsgate --port COM3
espflash monitor --port COM3 --non-interactive

# Fuzz smoke (nightly, run after touching PDU/URC/command parsers)
# Note: requires cargo-fuzz and Linux/macOS (Windows DLL issue with libFuzzer)
cargo +nightly fuzz run pdu_decode    -- -max_total_time=60
cargo +nightly fuzz run urc_parse     -- -max_total_time=60
cargo +nightly fuzz run command_parse -- -max_total_time=60
```

## Toolchain Setup (Windows)

```bash
# 1. Install Xtensa Rust toolchain
cargo install espup && espup install
# Sets up ~/.rustup/toolchains/esp + exports in ~/export-esp.ps1

# 2. Set environment before building (run each session)
export LIBCLANG_PATH="$HOME/.rustup/toolchains/esp/xtensa-esp32-elf-clang/esp-clang/bin/libclang.dll"
export PATH="$HOME/.rustup/toolchains/esp/xtensa-esp32-elf-clang/esp-clang/bin:$PATH"
export PATH="$HOME/.rustup/toolchains/esp/xtensa-esp-elf/bin:$PATH"
export PATH="$HOME/AppData/Local/Programs/Python/Python312:$PATH"

# 3. Flash tool
cargo install espflash ldproxy
```

## Architecture

Full design: `rfc/0001-foundation.md` (shared with humans — agents implement, humans review)

The system is built around four traits. All business logic depends only on these;
nothing in `bridge/`, `commands/`, or `sms/` imports a concrete implementation.

| Trait | Defined in | Abstracts |
|-------|-----------|-----------|
| `ModemPort` | `modem/mod.rs` | AT commands, URC polling, PDU SMS send |
| `Messenger` | `im/mod.rs` | send IM message, poll for inbound messages |
| `Store` | `persist/mod.rs` | NVS key-value persistence |
| `Board` | `boards/mod.rs` | pin layout, power-on sequence, builds `ModemPort` |
| `Command` | `commands/mod.rs` | single bot command (name, description, handler) |

`Board` is used only during startup in `main.rs` to produce a `ModemPort`.
After that, `Board` disappears from the call graph entirely.

Large implementations are split into subdirectories (`modem/a76xx/`, `im/telegram/`,
`commands/builtin/`). Small ones are single files. Trait definitions always live
in the parent `mod.rs`.

NVS stores exactly four keys: `im_cursor` (i64), `reply_map` (blob), `block_list` (blob),
`fwd_enabled` (bool). Configuration lives in compile-time `config.toml`, not NVS.

## Task Recipes

### Add a bot command (hard cap: 10 — check count first)
1. `src/commands/builtin/<name>.rs` — implement `Command` trait
2. `src/commands/builtin/mod.rs` — `pub use <name>::<Name>Command;`
3. `src/main.rs` `build_registry()` — `registry.register(Box::new(<Name>Command));`
4. `tests/command_dispatch.rs` — add a test using `RecordingMessenger` + `MemStore`
5. `cargo test --no-default-features --features testing --test command_dispatch`

### Add a board
1. `src/boards/<board>.rs` — implement `Board` trait
2. `src/boards/mod.rs` — `#[cfg(feature = "board_<board>")]` conditional export
3. `Cargo.toml` — add feature `board_<board>`
4. `config.toml.example` — document default pins for this board

### Add a test scenario
1. Pick or create a file in `tests/`
2. `Scenario::new("...").modem_urc(...).expect_im_sent(...).run()`
3. Real hardware recording → add `serial_capture/<description>.txt`

## Key Invariants

Verify after every change:

- PDU roundtrip: `encode(decode(x)) == x`
- Blocked numbers produce zero IM messages
- `FakeClock` u32 wraparound fires all timers correctly
- `ScriptedModem` unconsumed steps → test failure (no silent pass)
- Command count ≤ 10, or exception documented in `rfc/0001-foundation.md §4.2`
- NVS key set unchanged (4 keys only), or updated in `rfc/0001-foundation.md §4.3`

## sdkconfig.defaults Known Quirk

`ESP_IDF_SDKCONFIG_DEFAULTS` in `.cargo/config.toml` must be an **absolute path**. A relative
path is resolved against the esp-idf-sys crate directory in `~/.cargo/registry`, not the project
root, and silently produces the wrong (default) sdkconfig values.

If you change `sdkconfig.defaults` and the change doesn't seem to take effect, delete the cached
sdkconfig to force kconfgen to regenerate from scratch:
```bash
rm /c/t/xtensa-esp32-espidf/release/build/esp-idf-sys-*/out/sdkconfig
```
Then rebuild. The `sdkconfig.defaults` is applied as a *seed* (lower priority than existing
sdkconfig), so deleting the cache is required for changes to take effect.

## Boot Sequence Timing

Expected boot log milestones on T-A7670X cold start:
- `t≈645ms`: smsgate starting
- `t≈3545ms`: RESET_PIN configured
- `t≈6745ms`: Board power-on sequence complete (modem booted)
- `t≈12900ms`: Modem responded to AT probe
- `t≈15500ms`: **Network registered** (typical; within 30s window)
- `t≈19500ms`: WiFi DHCP IP assigned
- `t≈21000ms`: Sweeping existing SMS
- `t≈22000ms`: smsgate ready

If network registration doesn't appear within 30s, a warning is logged and boot continues.
SMS delivery still works — the modem registers in the background.

## Modem Driver Notes

**`is_urc` deliberately excludes `+CREG:` / `+CGREG:` / `+CEREG:`**. With `AT+CREG=0`
(default — no URC mode), these prefixes appear only as responses to `AT+CREG?`. If they were
classified as URCs, `send_at("+CREG?")` would siphon the response into the URC buffer and
registration checks would always return `false`. Do not add them back to `is_urc` unless
`AT+CREG=1` (or `=2`) is also added to the modem init sequence.

**`AT+CNMI=2,1,0,0,0`** (store + `+CMTI` notify) is the required setting. The alternative
`mt=2` (direct `+CMT` delivery) requires two-line URC parsing that is not implemented.

**`AT+CPMS` and storage memory**: On the T-A7670X hardware, `AT+CPMS?` returns `+CMS ERROR`
(the SIM doesn't support SMS management queries). The modem defaults to `"ME"` (device flash)
for all three memory slots. `+CMTI` notifications will say `+CMTI: "ME",<index>`. The modem
driver passes the memory name through `Urc::NewSms { mem, index }` and calls `AT+CPMS=<mem>`
before each `AT+CMGR` to guarantee the read uses the correct storage. SMS is deleted only after
a successful Telegram forward — if `forward_sms` fails the slot stays occupied and sweep on
next boot retries. Do not add `AT+CPMS="SM","SM","SM"` to modem init: it silently triggers
CMTI notifications for all stored SMS which can overflow the 256-byte UART Rx buffer during init.

## Forbidden Patterns

- `>=` / `<` on raw `u32` timestamps — use `elapsed_since()` / `is_past()` only
- Literal WiFi password, bot token, or pin numbers anywhere in `src/`
- Importing `im::telegram` (or any concrete backend) from `bridge/`, `commands/`, `sms/`, `persist/`
- Adding a fifth NVS key without updating `rfc/0001-foundation.md §4.3`
- ASCII art diagrams in documentation — use Mermaid instead
- Adding `+CREG:` back to `is_urc` without also enabling `AT+CREG=1` in modem init
