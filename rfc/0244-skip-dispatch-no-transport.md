---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0244: Skip +CMTI dispatch from drain loop when transport is kNone

## Motivation

When `activeTransport == kNone` (no WiFi, no cellular), every `+CMTI`
URC processed by the drain loop causes `handleSmsIndex` to attempt a
Telegram send.  `doSendMessage` fails immediately (no transport set →
early return 0), which calls `noteTelegramFailure()` and increments the
consecutive-failure counter.  After 8 failures the device reboots.  On
the next boot the deferred-sweep flag (RFC-0238) prevents the boot sweep
from immediately firing 8 more failures.  But new `+CMTI` URCs from
newly arriving SMS start the count again.  If SMS keep arriving while
the device has no connectivity, it is stuck in a continuous reboot loop.

## Plan

In the drain loop, when `+CMTI: <idx>` is received:
- If `activeTransport == kNone`: log the skip and return without
  dispatching.  The SMS is already stored on the SIM; it will be
  forwarded by `sweepExistingSms()` as soon as transport becomes
  available (RFC-0243 sweeps on every transport establishment event).
- If `activeTransport != kNone`: dispatch normally (existing path).

## Notes for handover

The RFC-0236 piggybacked-URC dispatch in the 30 s status-refresh block
intentionally does NOT apply the same kNone guard — that block only
runs when the modem is responding (implying the 30 s status refresh
completed), and by that time there's usually a transport in place.  If
there really is no transport but the piggybacked +CMTI fires, the
worst-case is one failure per 30 s refresh — too slow to exhaust the
8-failure counter.  The drain loop is the only place where a rapid
burst of +CMTI URCs could exhaust the counter.
