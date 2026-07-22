# smsgate

ESP32 firmware in Rust that bridges SMS/calls and Telegram. Receives SMS on a cellular modem, forwards to Telegram; reply to a Telegram message to send an SMS back.

## Hardware

Any ESP32 board paired with an AT-command cellular modem is supported via the `Board` trait.
Reference hardware: **LilyGo T-A7670X R2** and **T-A7670X S3 Standard (H799)**
with an A7670G LTE modem.

- A nano-SIM card with SMS service

## Features

- Two-way SMS forwarding (SMS to Telegram, Telegram reply to SMS)
- Incoming call notification with auto-hangup
- Multipart SMS reassembly (concatenated SMS)
- PDU-mode SMS encoding/decoding (GSM-7 + UCS-2)
- Bot commands: `/status`, `/send`, `/block`, `/unblock`, `/pause`, `/resume`, `/log`, `/restart`, `/update`
- Flash-backed event log with HTML rendering and Telegram button pagination
- Telegram document OTA: send a `.bin` with caption `/ota`
- Separate software-only and compiled-config OTA image variants
- i18n: English and Chinese (compile-time locale selection, zero overhead)
- NVS persistence for cursor, reply mapping, and block list
- Outbound SMS queue with exponential-backoff retry
- Hardware watchdog (120s timeout)
- Build commit hash embedded in `/status` output

## Quick Start

```bash
# 1. Install Xtensa Rust toolchain
cargo install espup && espup install

# 2. Copy and fill in config
cp config.toml.example config.toml
# Edit config.toml with your WiFi credentials, Telegram bot token, and chat ID

# 3. Run host tests (no hardware needed)
cargo test --no-default-features --features testing

# 4. Build firmware
cargo +esp build --release --target xtensa-esp32-espidf
# Windows note: ESP-IDF has path-length limits. Set a short target dir:
#   CARGO_TARGET_DIR=C:\t cargo +esp build --release --target xtensa-esp32-espidf

# 5. Flash
cargo install espflash
espflash flash target/xtensa-esp32-espidf/release/smsgate --partition-table partitions_ota_4m.csv --port <PORT>
# PORT is /dev/ttyUSB0 (Linux), /dev/cu.wchusbserial* (macOS), or COM3 (Windows)
```

For the ESP32-S3 H799 board, set `uart_tx=4`, `uart_rx=5`, and `pwrkey=46`
in `config.toml`, then build and flash on Windows with:

```powershell
$env:ESP_IDF_SDKCONFIG_DEFAULTS = "$PWD\sdkconfig.esp32s3.defaults"
$env:SMSGATE_FLASH_LAYOUT = "16m"
$env:CARGO_TARGET_DIR = "C:\ts3"
cargo +esp build --release --target xtensa-esp32s3-espidf
espflash flash C:\ts3\xtensa-esp32s3-espidf\release\smsgate `
  --partition-table partitions_ota_16m.csv --port COM3
```

Partition layouts are fixed per supported board: `partitions_ota_4m.csv` for
T-A7670X R2 and `partitions_ota_16m.csv` for T-A7670X S3 Standard H799. Both
include a `log_ring` partition used by `/log`; the S3 layout provides 512 KiB.

## Telegram OTA

Generate both S3 OTA app images on Windows:

```powershell
.\tools\build-ota-images.ps1 -Board s3 -OutDir C:\ts3\ota
```

Linux/macOS use `./tools/build-ota-images.sh s3 ./ota`. The generated
`software-only` image preserves runtime credentials in NVS; `with-config`
applies the credentials compiled from `config.toml` on first boot. Send the
chosen `.bin` from a trusted Telegram chat with caption `/ota`.

## Configuration

`config.toml` provides compile-time defaults. WiFi, Telegram and APN credentials
also live in the seven existing `smsgcfg` NVS keys and may be provisioned over
serial. `SMSGATE_APPLY_COMPILED_CONFIG=0` preserves NVS values; `=1` replaces
them with the compiled defaults. See [`config.toml.example`](config.toml.example).

To build with Chinese UI strings, add to your `config.toml`:

```toml
[ui]
locale = "zh"
```

## Design Tradeoffs

**`serde_json` for Telegram API parsing** — The Telegram HTTP layer uses `serde_json`, which requires heap allocation. This is a deliberate tradeoff: the ESP32 has ample SRAM (320 KB + optional PSRAM), a typical Telegram API response is a few kilobytes, and `serde-json-core` (the `no_std` alternative) would add significant implementation complexity for marginal gain. If you port this to a more constrained MCU, swapping out `im/telegram/` is the only change needed.

**Configuration boundaries** — Hardware pins and locale remain compile-time.
Operational credentials can be retained across software-only OTA updates or
intentionally replaced by a with-config image.

## Architecture

The system is built around four core traits. All business logic depends only on these abstractions:

| Trait | Abstracts |
|-------|-----------|
| `ModemPort` | AT commands, URC polling, PDU SMS send |
| `MessageSink` / `MessageSource` | Send/poll IM messages (Telegram) |
| `Store` | NVS key-value persistence |
| `Command` | Single bot command (name, description, handler) |

## USB Driver

The USB interface varies by board. T-A7670X R2 uses a **CH9102** bridge;
H799 uses the ESP32-S3 native USB Serial/JTAG port labeled **ESP-USB**.

- **Linux**: typically works out of the box (`/dev/ttyUSB0`); if not, load the appropriate kernel module (`ch341`, `cp210x`, etc.)
- **macOS**: install the driver matching your chip (e.g. [CH34x](https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html) for CH9102) and approve the kext in System Settings > Privacy & Security
- **Windows**: usually auto-detected; if not, install from the chip vendor (e.g. [WCH](https://www.wch-ic.com/downloads/CH343SER_ZIP.html) for CH9102)

## License

[MIT](LICENSE)
