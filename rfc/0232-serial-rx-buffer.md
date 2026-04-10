---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0232: Increase SerialAT RX buffer

## Motivation

The ESP32 Arduino `HardwareSerial` (Serial1) default RX buffer is 256 bytes.
A single `+CMTI: "SM",<idx>\r\n` URC is ~22 bytes. With the default buffer,
only ~11 URCs can queue before the buffer overflows and earlier URCs are
silently discarded. During slow operations (TLS handshake, `sendBotMessage`,
`doSendMessage` HTTP round-trip), if more than ~11 SMS arrive simultaneously,
some +CMTI URCs are lost. The SMS stays on the SIM (not deleted) so it is
recovered at the next `sweepExistingSms()` (boot or WiFi reconnect), but
timeliness is hurt.

## Plan

Call `Serial1.setRxBufferSize(2048)` immediately before `SerialAT.begin()` in
`setup()`. This reserves 2 KB of RAM for the UART RX buffer. At 22 bytes per
+CMTI URC, this buffers ~90 concurrent arrival notifications — more than any
real SIM card can generate in a burst.

The ESP32 has 520 KB of SRAM with ~360 KB available to user code (after
WiFi stack, TLS, etc.) and we currently use ~18% of the 3 MB flash.
2 KB of RAM is negligible.

## Notes for handover

`setRxBufferSize` must be called BEFORE `begin()`. On ESP32 Arduino, calling
it after `begin()` has no effect. The function is `Serial1.setRxBufferSize(n)`
where `Serial1` is what `utilities.h` maps `SerialAT` to.
