---
status: deferred
created: 2026-04-09
---

# RFC-0006: MODEM_RING_PIN hardware interrupt + ESP32 sleep

## Motivation

Two possible wins, one real, one cosmetic:

1. **Power savings (the real win, conditional on battery operation).**
   Today the board is USB-powered, so idle current draw is somebody
   else's problem. If the user ever moves to battery operation (this
   project was forked from dev boards that explicitly support LiPo),
   the 240MHz ESP32 running `loop()` with a 50ms `delay()` is the
   biggest continuous draw on the system after the modem itself.
   Light-sleep between events could cut idle draw by roughly an order
   of magnitude.
2. **Architectural tidiness (cosmetic).** Polling `SerialAT.available()`
   every 50ms to wait for `+CMTI` / `RING` is the kind of thing that
   reads weird in code review. Using an ISR + FreeRTOS primitive to
   sleep the task until the modem actually has something to say is
   "the right way" regardless of power.

We are **deferring** this RFC because the user is USB-powered and has
not asked for battery operation. This document exists so that if the
answer ever changes, future-us or future-agent doesn't have to
re-derive the design.

## Current state

- `MODEM_RING_PIN` is `#define`'d in `utilities.h` (GPIO 33 on the
  T-A7670X variant used). It is pinMode'd to `INPUT_PULLUP` in
  `setup()` of `main.cpp`, and then **never read**. The hardware path
  for hardware-interrupt-driven event handling exists but is entirely
  unused.
- `loop()` polls the UART RX buffer via `SerialAT.available()` and
  `delay(50)`s. On modem event (`+CMTI`, `RING`) the data is already
  in the ESP32's UART RX FIFO by the time we poll; the 50ms delay
  just adds latency, it doesn't lose events.
- The upstream `examples/WakeupByRingOrSMS/WakeupByRingOrSMS.ino`
  shows how to use `MODEM_RING_PIN` as an EXT0 deep-sleep wake source.
  That example is for a very different use-case (ESP mostly asleep,
  waking only to note that *something* came in) and its model does
  not fit a keep-alive Telegram connection, but it confirms that
  the pin is wired up and active-low on ring.

## Plan (only if battery operation becomes a requirement)

### Interrupt-driven drain loop

1. Create a FreeRTOS binary semaphore `ringSem`.
2. `attachInterrupt(digitalPinToInterrupt(MODEM_RING_PIN), ringIsr, FALLING);`
   with an ISR that does nothing but `xSemaphoreGiveFromISR(ringSem, ...)`.
3. Main-task `loop()` becomes:
   ```cpp
   if (xSemaphoreTake(ringSem, pdMS_TO_TICKS(30000))) {
     drainSerialAt();
   } else {
     // periodic housekeeping (WiFi reconnect, watchdog pet, etc.)
   }
   ```
4. Keep a *timed* fallback drain on the 30s timeout so a missed ISR
   (unlikely but possible) cannot cause an event to be lost
   indefinitely.

### Light sleep (easier, keeps TLS alive)

1. When `ringSem` is not signalled and no housekeeping is pending,
   call `esp_light_sleep_start()` with the `MODEM_RING_PIN` configured
   as a GPIO wake source and the UART RX pin also configured as a
   wake source (`uart_set_wakeup_threshold` / `esp_sleep_enable_uart_wakeup`).
2. On wake, the `WiFiClientSecure` keep-alive connection is usually
   still valid because the AP has not yet timed it out. If it has, the
   existing `keepTelegramClientAlive()` reconnect path handles it.
3. Current drops from ~80mA active to ~0.8mA in light sleep (rough
   ESP32 figures) while the modem continues to hold the network
   registration.

### Deep sleep (harder, do not attempt first)

Deep sleep wipes RAM. That means:

- `WiFiClientSecure` TLS session is gone; handshake cost is paid on
  every wake (~1–2s and ~40KB of RAM during handshake)
- Telegram keep-alive connection is gone
- WiFi DHCP lease may or may not be gone
- Depending on how long we sleep, the LTE registration may have been
  dropped by the network, requiring a re-register (which can take
  tens of seconds)

Deep sleep would only make sense with very long idle windows
(minutes to hours) where the handshake cost is amortized. Do not
attempt as the first cut. Start with light sleep.

## Notes for handover

- **This is mostly cosmetic unless the user wants battery operation.**
  On USB power, the benefit of ISR-driven waking is a cleaner
  architecture and ~50ms lower event latency. That is not a good
  reason on its own to take on the complexity. Do not implement until
  there is a concrete power requirement.
- `HardwareSerial` on ESP32 already uses RX interrupts internally to
  drain the UART into its ring buffer. The ISR we would add for
  `MODEM_RING_PIN` is **not** about catching UART bytes — it's about
  waking the main task from sleep. `SerialAT.available()` works fine
  during normal operation precisely because the UART RX path is
  already interrupt-driven underneath.
- The `MODEM_RING_PIN` signal from the A76xx is active-low. The
  modem asserts it for the duration of the ring, same as landline
  telephony. That means a simple `FALLING` edge ISR catches the
  leading edge of each ring, which is all we need.
- If deep sleep is ever attempted, follow the pin-hold pattern from
  `examples/WakeupByRingOrSMS/WakeupByRingOrSMS.ino`:
  `gpio_hold_en(BOARD_POWERON_PIN)` etc. so the modem doesn't
  power-cycle on ESP wake. That file is the working reference.
- Cross-reference with RFC-0004 (cellular fallback): if the device is
  using cellular as its primary uplink, keep-alive TLS over the modem
  socket is materially more expensive to re-establish than
  `WiFiClientSecure`. Sleep strategy changes under that mode.
