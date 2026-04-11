# smsgate — Rust rewrite

ESP32 firmware that bridges SMS and IM (Telegram and others).
Hardware: LilyGo T-A7670X (ESP32 + A7670G modem).

**This branch (`rust-rewrite`) is a clean-slate Rust port. Scaffold only — not yet functional.**
The working C++ firmware lives on `main` / `stable`.

## Design documents

| Document | Contents |
|----------|---------|
| [`rfc/0001-foundation.md`](rfc/0001-foundation.md) | Scope, architecture, trait design, decision log |
| [`rfc/0002-agent-harness.md`](rfc/0002-agent-harness.md) | Agent infrastructure design (CLAUDE.md, Skills, sub-agents) |

## Build

```bash
# One-time toolchain setup
cargo install espup && espup install
# macOS: export LIBCLANG_PATH=$(brew --prefix llvm)/lib

# Copy and fill in config
cp config.toml.example config.toml

# Host tests (no hardware needed)
cargo test --workspace --exclude fuzz

# Firmware
cargo build --release --target xtensa-esp32-espidf

# Flash
cargo espflash flash --release --port /dev/cu.wchusbserial*
```

## USB driver

The T-A7670X uses a CH9102 USB bridge.
Mac driver: https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html
After installing, approve the kext in System Settings → Privacy & Security.
