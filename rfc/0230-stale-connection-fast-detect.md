---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0230: Fast stale keep-alive connection detection

## Motivation

`RealBotClient::doSendMessage` and `pollUpdates` both use HTTP keep-alive to
reuse an existing TLS session. If the server closes the connection (load
balancer timeout, server restart) between requests, `WiFiClientSecure::connected()`
still returns `true` (TCP half-open) until a read attempt is made. The first
read attempt — `transport_->readStringUntil('\n')` — then waits for the full
`WiFiClientSecure.setTimeout()` = 15 000 ms before returning an empty string,
blocking the entire `loop()` for 15 seconds.

During those 15 s: incoming +CMTI URCs queue in the SerialAT buffer (which
overflows at ~128 bytes), RING / +CLIP are lost, and the watchdog must fire
before recovery.

## Plan

1. After sending an HTTP request, poll `transport_->available()` for up to
   **3 000 ms** before calling `readStringUntil`. If no byte arrives in 3 s,
   the connection is stale: call `transport_->stop()` and return
   immediately (0 / false). On the next tick, `keepTransportAlive` will
   reconnect.

2. Apply to both `doSendMessage` and `pollUpdates`.

3. Emit a `Serial.println` so the serial log shows the stale-connection event.

## Notes for handover

3 s is generous — Telegram's API normally responds in < 200 ms. False stale
detection (slow-but-working connection) is extremely unlikely at 3 s.
The WDT reset at the top of `doSendMessage` (RFC-0028) means the 3 s wait does
not threaten the 120-s WDT.
