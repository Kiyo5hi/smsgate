---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0231: HTTP header read deadline

## Motivation

After RFC-0230 the initial status-line stall is bounded at 3 s.
However, the header drain loop in `doSendMessage` and `pollUpdates` has
no deadline: each `readStringUntil('\n')` call can still block for
`WiFiClientSecure.timeout()` = 15 s if the server sends the status line
then stalls mid-headers (e.g. during a partial load-balancer restart).
With 5 typical headers, this can still block the main loop for 75 s.

## Plan

Add a `headerDeadline = millis() + 5000` before each header drain loop.
On each iteration, if `millis() > headerDeadline`, stop the transport
and return failure immediately.

Apply to both `doSendMessage` and `pollUpdates`.

## Notes for handover

5 s is very generous — normal Telegram API headers arrive within 200 ms
of the status line. The transport stop ensures `keepTransportAlive`
reconnects on the next call.
