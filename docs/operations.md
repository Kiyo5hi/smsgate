# Operations

## Flash

Confirm chip, flash size, configuration, port and partition layout using
[hardware.md](hardware.md). Stop any serial monitor holding that port.
Use the built ELF path and selected CSV:

```sh
espflash flash <ELF> --port <PORT> --partition-table <BOARD_CSV> --target-app-partition ota_0
```

Changing partition layout is a separate migration: inspect existing offsets
before flashing; NVS and logs may be affected. Do not erase NVS as routine setup.
Pass the board CSV directly; a generated partitions_ota.bin is not required
for this command.

## OTA

```powershell
.\tools\build-ota-images.ps1 -Board s3 -OutDir .local/ota
```

Linux/macOS: `./tools/build-ota-images.sh s3 .local/ota`.
Use r2 for the R2 variant. Check pins/config before building.

Send the selected app image from a trusted Telegram chat with caption /ota.
The /update command instead uses the configured HTTPS URL; there is no current
automatic nightly publisher in this repository. Use the image for the exact
chip and compatible partition layout.

Software-only preserves NVS credentials. With-config applies compiled defaults.
Treat builds made with real config.toml as sensitive, including software-only
artifacts, until their contents are verified. Keep them in private storage.

OTA uses an inactive app slot. Confirmation mode affects rollback:
automatic confirmation occurs during startup; manual mode requires confirmation.
Do not interpret rollback support as proof of successful SMS delivery.

## Acceptance and Records

1. Record device ID, source commit and dirty status, target/layout, artifact
   SHA-256, provisioning mode and timestamp with timezone.
2. Confirm clean boot, SIM ready, network registration, expected CNMI, selected
   storage, WiFi and smsgate ready.
3. Send an external SMS and confirm Telegram delivery.
4. Send /status from a trusted account and confirm the caller receives the reply.
5. For recovery changes, observe the recovery timer and test the failure path
   where practical. Record what was and was not exercised.

Use [device.example.md](device.example.md) in private storage. A commit hash
alone cannot identify firmware built from a dirty tree.

Logs are flash-backed and retained until capacity-based ring overwrite; they
do not have a fixed number-of-days retention guarantee. /log pages contain five
entries at the current base commit. Keep raw captures private and share only
redacted, minimal evidence.
