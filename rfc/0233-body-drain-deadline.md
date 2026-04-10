---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0233: Reduce body drain deadline and force-stop on timeout

## Motivation

`doSendMessage` had an 8-second body drain deadline. Telegram's sendMessage
response is typically 200-500 bytes and arrives within 200 ms; 8 s is
unnecessarily permissive and keeps the main loop busy for up to 8 s on a
slow/hung connection.

Additionally, when the deadline expired without a complete body, the code
returned without stopping the transport — leaving stale bytes in the TLS
buffer that would corrupt the next HTTP request.

## Plan

1. Reduce `doSendMessage` body drain deadline from 8 s to 4 s.
2. On deadline expiry (partial body), call `transport_->stop()` so
   `keepTransportAlive` reconnects on the next call. Fall through to
   check `"ok":true` in the partial body — if Telegram confirmed the
   send before the connection died, we can still return success.
3. Same `transport_->stop()` on `pollUpdates` body deadline expiry.

## Notes for handover

4 s is still 20× the typical response time. The force-stop is the key
safety property: after a timeout, the connection is always in a clean
state for the next request.
