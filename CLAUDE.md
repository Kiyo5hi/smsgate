# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 firmware in Rust — bridges SMS and IM (Telegram and others); two-way forwarding.
Designed to support any ESP32 board paired with an AT-command cellular modem via the `Board` trait.
Reference hardware: LilyGo T-A7670X (A7670G modem, CH9102 USB bridge).
This branch (`main`) is hardware-tested and boots to working state on real hardware.

## Commands

```bash
# Host tests — no hardware needed; use after every change
cargo test --no-default-features --features testing

# Single test file
cargo test --no-default-features --features testing --test <name>

# Build firmware (requires Xtensa toolchain — see Toolchain Setup below)
cargo +esp build --release --target xtensa-esp32-espidf
# Windows: ESP-IDF has path-length limits; use a short target dir:
#   CARGO_TARGET_DIR=C:\t cargo +esp build --release --target xtensa-esp32-espidf

# Flash + monitor
# PORT: /dev/ttyUSB0 (Linux), /dev/cu.wchusbserial* (macOS), COM3 (Windows)
espflash flash target/xtensa-esp32-espidf/release/smsgate --port <PORT>
espflash monitor --port <PORT> --non-interactive

# Fuzz smoke (nightly, run after touching PDU/URC/command parsers)
# Note: requires cargo-fuzz and Linux/macOS (Windows DLL issue with libFuzzer)
cargo +nightly fuzz run pdu_decode    -- -max_total_time=60
cargo +nightly fuzz run urc_parse     -- -max_total_time=60
cargo +nightly fuzz run command_parse -- -max_total_time=60
```

## Toolchain Setup

```bash
# 1. Install Xtensa Rust toolchain (all platforms)
cargo install espup && espup install
# Linux/macOS: source ~/export-esp.sh in each session (or add to shell profile)
# Windows:     source ~/export-esp.ps1 in each session

# 2. Flash tool
cargo install espflash ldproxy
```

On **Windows**, if `espup install` does not set the environment automatically,
set these before building:

```powershell
$env:LIBCLANG_PATH = "$env:USERPROFILE\.rustup\toolchains\esp\xtensa-esp32-elf-clang\esp-clang\bin\libclang.dll"
$env:PATH = "$env:USERPROFILE\.rustup\toolchains\esp\xtensa-esp32-elf-clang\esp-clang\bin;$env:USERPROFILE\.rustup\toolchains\esp\xtensa-esp-elf\bin;$env:PATH"
```

## Architecture

The system is built around core traits. All business logic depends only on these;
nothing in `bridge/`, `commands/`, or `sms/` imports a concrete implementation.

| Trait | Defined in | Abstracts |
|-------|-----------|-----------|
| `ModemPort` | `modem/mod.rs` | AT commands, URC polling, PDU SMS send |
| `MessageSink` | `im/mod.rs` | outbound delivery (Telegram, webhook, MQ, etc.) |
| `MessageSource` | `im/mod.rs` | inbound command polling (Telegram only) |
| `Store` | `persist/mod.rs` | NVS key-value persistence |
| `Board` | `boards/mod.rs` | pin layout, power-on sequence, builds `ModemPort` |
| `Command` | `commands/mod.rs` | single bot command (name, description, handler) |

`FanoutSink` wraps multiple `MessageSink`s and delivers to all of them.
The first sink is the primary (its `MessageId` is used for reply routing);
the rest are fire-and-forget.

`Board` is used only during startup in `main.rs` to produce a `ModemPort`.
After that, `Board` disappears from the call graph entirely.

Large implementations are split into subdirectories (`modem/a76xx/`, `im/telegram/`,
`commands/builtin/`). Small ones are single files. Trait definitions always live
in the parent `mod.rs`.

NVS stores exactly four keys: `im_cursor` (i64), `reply_map` (blob), `block_list` (blob),
`fwd_enabled` (bool). Configuration lives in compile-time `config.toml`, not NVS.

### OTA Updates

Dual-partition OTA via ESP-IDF (`partitions_ota.csv`). The `/update` command
downloads firmware from the configured HTTPS URL, writes to the inactive slot,
and reboots. Rollback is automatic if the new firmware fails to boot.
Configure via `[ota]` in `config.toml`.

First flash must include the partition table:
```bash
espflash flash smsgate --port <PORT> --partition-table partitions_ota.bin
```

`partitions_ota.bin` is not tracked in git (generated artifact). Regenerate it from
the CSV whenever the partition layout changes:
```bash
# Using ESP-IDF Python environment:
python $IDF_PATH/components/partition_table/gen_esp32part.py partitions_ota.csv partitions_ota.bin
# Or using the PlatformIO Python on Windows:
"/c/Users/$USER/.platformio/penv/Scripts/python.exe" \
  ~/.platformio/packages/framework-espidf/components/partition_table/gen_esp32part.py \
  partitions_ota.csv partitions_ota.bin
```

### Nightly CI / OTA Release

`.github/workflows/nightly.yml` builds firmware every night and publishes a
`nightly` GitHub Release. The released `smsgate.bin` is an OTA-ready app image.

Required GitHub Secrets (Settings → Secrets and variables → Actions):

| Secret | Example |
|--------|---------|
| `WIFI_SSID` | `MyNetwork` |
| `WIFI_PASSWORD` | `hunter2` |
| `TELEGRAM_BOT_TOKEN` | `123456:ABC-DEF...` |
| `TELEGRAM_CHAT_ID` | `8024680950` |
| `UI_LOCALE` *(optional)* | `zh` (default) or `en` |

The OTA URL in the built firmware points to the same repo's nightly release,
so `/update` always fetches the latest nightly build.

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

## Hardware Testing

**Every change must be tested on real hardware before the task is considered done.**
Host tests (`cargo test`) verify logic; they cannot catch UART timing issues, modem
power sequencing, NVS partition behaviour, or FreeRTOS scheduling interactions.

Minimum verification on board after any change:
1. Flash and confirm clean boot log (see Boot Sequence Timing below)
2. Send an SMS to the device — confirm it forwards to Telegram
3. Send `/status` from Telegram — confirm it replies

## Key Invariants

Verify after every change:

- PDU roundtrip: `encode(decode(x)) == x`
- Blocked numbers produce zero IM messages
- `FakeClock` u32 wraparound fires all timers correctly
- `ScriptedModem` unconsumed steps → test failure (no silent pass)
- Command count ≤ 10 (hard cap — adding one requires removing one)
- `"smsgate"` NVS key set unchanged (4 keys only: im_cursor, reply_map, block_list, fwd_enabled)
- `"smsgcfg"` NVS credential keys unchanged (7 keys: wifi_ssid/pass, bot_token, chat_id, apn/user/pass)

## sdkconfig.defaults Known Quirk

`ESP_IDF_SDKCONFIG_DEFAULTS` in `.cargo/config.toml` must be an **absolute path**. A relative
path is resolved against the esp-idf-sys crate directory in `~/.cargo/registry`, not the project
root, and silently produces the wrong (default) sdkconfig values.

If you change `sdkconfig.defaults` and the change doesn't seem to take effect, delete the cached
sdkconfig to force kconfgen to regenerate from scratch:
```bash
rm target/xtensa-esp32-espidf/release/build/esp-idf-sys-*/out/sdkconfig
# (adjust path if you used a custom CARGO_TARGET_DIR)
```
Then rebuild. The `sdkconfig.defaults` is applied as a *seed* (lower priority than existing
sdkconfig), so deleting the cache is required for changes to take effect.

## Partition CSV Build Ordering (Known Issue)

`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` in `sdkconfig.defaults` is a relative path resolved
against esp-idf-sys's `out/` directory (e.g. `C:\t\...\build\esp-idf-sys-<hash>\out\`).
`build.rs` copies `partitions_ota.csv` there, but Cargo runs esp-idf-sys's build script
**before** smsgate's — so on a fresh build the cmake ninja step tries to find the CSV before
the copy has happened.

**Normal builds (Cargo.lock pinned, no `cargo update`) are unaffected** — esp-idf-sys is cached
and the copy runs cleanly on smsgate's build.rs.

The failure only occurs the **first time** after `cargo update` bumps esp-idf-sys to a new
version (new hash → new out dir → CSV missing). Symptom:
```
ninja: error: '…/esp-idf-sys-<hash>/out/partitions_ota.csv', needed by 'partition-table.bin', missing
```

**Recovery (Windows, `CARGO_TARGET_DIR=C:\t`):**
```powershell
Copy-Item partitions_ota.csv `
  (Get-ChildItem C:\t\xtensa-esp32-espidf\release\build\esp-idf-sys-*\out | Select-Object -First 1).FullName
```
```bash
# bash equivalent
cp partitions_ota.csv C:/t/xtensa-esp32-espidf/release/build/esp-idf-sys-*/out/
```
Then re-run `cargo +esp build`. The copy is permanent for that esp-idf-sys hash; subsequent
builds succeed automatically.

## Boot Sequence Timing

Milestones below are for the reference board (T-A7670X) on a cold start; exact timings vary by board and modem:
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

**`AT+CPMS` and storage memory**: Some modems (e.g. A7670G on T-A7670X) return `+CMS ERROR`
to `AT+CPMS?` because the SIM doesn't support SMS management queries. When that happens the
modem defaults to `"ME"` (device flash) for all three memory slots. `+CMTI` notifications will say `+CMTI: "ME",<index>`. The modem
driver passes the memory name through `Urc::NewSms { mem, index }` and calls `AT+CPMS=<mem>`
before each `AT+CMGR` to guarantee the read uses the correct storage. SMS is deleted only after
a successful Telegram forward — if `forward_sms` fails the slot stays occupied and sweep on
next boot retries. Do not add `AT+CPMS="SM","SM","SM"` to modem init: it silently triggers
CMTI notifications for all stored SMS which can overflow the 256-byte UART Rx buffer during init.

## Forbidden Patterns

- `>=` / `<` on raw `u32` timestamps — use `elapsed_since()` / `is_past()` only
- Literal WiFi password, bot token, or pin numbers anywhere in `src/`
- Importing `im::telegram` (or any concrete backend) from `bridge/`, `commands/`, `sms/`, `persist/`
- Adding a fifth NVS key to `"smsgate"` without updating the Key Invariants section above
- ASCII art diagrams in documentation — use Mermaid instead
- Adding `+CREG:` back to `is_urc` without also enabling `AT+CREG=1` in modem init
