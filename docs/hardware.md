# Hardware

These are the repository's supported build combinations. Confirm the actual
board label, chip and flash size before selecting one.

| Setting | T-A7670X R2 | T-A7670X S3 Standard H799 |
| --- | --- | --- |
| ESP chip | ESP32 | ESP32-S3 |
| Modem | A7670G | A7670G |
| Flash layout | 4m | 16m |
| Rust target | xtensa-esp32-espidf | xtensa-esp32s3-espidf |
| SDK defaults | sdkconfig.defaults | sdkconfig.esp32s3.defaults |
| Partition CSV | partitions_ota_4m.csv | partitions_ota_16m.csv |
| UART TX / RX | 26 / 27 | 4 / 5 |
| PWRKEY | 4 | 46 |
| Host USB | CH9102 bridge | Native USB Serial/JTAG, ESP-USB |

Sources: [board implementations](../src/boards/mod.rs),
[example config](../config.toml.example), [build script](../build.rs).

Pins remain compile-time configuration. Selecting an OTA build target alone
does not replace checking the configured pins. Do not apply this matrix to
other LilyGo variants just because their modem name matches.

COM numbers and /dev paths are host observations, not device identities.
Record USB serial/board identifier privately and rediscover the current port.
