# Shared Agent Instructions

ESP32 Rust firmware bridging SMS/calls and IM. These rules apply to all coding
agents working in this repository.

## Start Here

Read [docs/handoff.md](docs/handoff.md), inspect `git status --short`, and consult
[docs/README.md](docs/README.md) for task-specific documentation. Code and current
CI define implemented behavior; dated observations do not prove current device state.
Update the handoff when work leaves unfinished changes or outstanding validation.

## Working Rules

- Preserve existing uncommitted changes. Do not mix unrelated work into a commit.
- Keep credentials out of Git, tool output, screenshots, and shared logs.
- Keep project knowledge in repository docs, not only chat or a user-level skill.
- Use relative links and parameterized commands; discover ports on each host.
- Business logic in bridge/, commands/, sms/, and persist/ depends on traits,
  not concrete IM backends or boards.
- Use Mermaid for diagrams, not ASCII art.
- No literal credentials or board pin numbers in business logic; board-specific
  pin handling belongs in the board implementation/configuration.

## Verification

Run after code changes:

```sh
cargo fmt --all -- --check
cargo clippy --no-default-features --features testing --all-targets -- -D warnings
cargo test --no-default-features --features testing
```

Firmware changes require real-hardware verification before being called complete:
clean boot, inbound SMS forwarded to Telegram, and a Telegram /status reply.
Record board, build identity, date, evidence, and outstanding checks.
Host tests do not establish UART, modem power, NVS, or scheduling correctness.
Documentation-only work requires link and accuracy checks, not a firmware flash.

After PDU/URC/command parser changes, also run the fuzz smoke commands in
[development.md](docs/development.md) on a supported host, or record the gap.

## Invariants

- Preserve PDU codec roundtrip behavior and its tests.
- Blocked numbers produce zero IM messages.
- Use elapsed_since()/is_past() for u32 timers, never raw timestamp comparisons.
- ScriptedModem tests must call check_consumed().
- Registered command count must remain <= 10.
- "smsgate" NVS: exactly im_cursor, reply_map, block_list, fwd_enabled.
- "smsgcfg" NVS: exactly wifi_ssid, wifi_pass, bot_token, chat_id, apn,
  apn_user, apn_pass. Update documentation/invariants if a schema change is authorized.
- Keep registration query responses out of is_urc unless registration URC mode
  and response parsing are deliberately redesigned together.
- Use stored SMS notifications (CNMI=2,1,0,0,0). Read the modem caveats in
  [troubleshooting.md](docs/troubleshooting.md) before changing SMS storage/init.

## Change Recipes

- Command: implement in src/commands/builtin/, export in mod.rs, register in
  main.rs, cover tests/test_commands.rs and tests/test_poller.rs as appropriate.
- Board: implement Board, update src/boards/mod.rs and startup selection,
  build.rs target cfg as needed, pins/example config, build tools and partitions.
  Current R2/S3 selection uses esp32s3 cfg, not board_* Cargo features.
- Scenario: extend tests/ using ScriptedModem/Scenario. Store only redacted
  diagnostic excerpts in shared docs; raw captures belong in ignored .local/.
