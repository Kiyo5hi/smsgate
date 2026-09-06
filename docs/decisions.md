# Design Decisions

## Preserve LTE Registration While Guarding User Data

The current A76xx guard deactivates user context CID 1 and preserves the
network-managed context CID 8 observed on the reference setup. Whole-domain
detachment via CGATT=0 can also prevent LTE SMS.

This is modem/network-specific behavior, not a guarantee of no roaming charges
or proof of CID meanings on all networks. Keep cellular fallback explicitly
configured and validate with real carrier observations.

Source: [A76xx driver](../src/modem/a76xx/mod.rs), commit 5c86eeb.

## Recover Stored SMS Without Restart

A five-minute CNMI check and ME sweep provide a fallback for missed notification
or a failed prior read/forward. Healthy notifications remain the immediate path.
This recovery is currently an uncommitted change; see [handoff.md](handoff.md).

Five minutes is the nominal scan interval while the main loop and modem are
responsive, not a guaranteed maximum end-to-end delivery latency.

## Separate Shared Knowledge from Deployment Secrets

Repository docs own reusable procedures. Private device records own identities
and deployments. Credentials use encrypted storage. Machine paths and ports
are detected locally. See [sharing.md](sharing.md).

## Keep Backend Dependencies Out of Business Logic

Traits isolate boards and IM transports. serde_json is used for Telegram parsing
on this std/heap-based ESP32 application. Replacing the parser or adding a board
should not require coupling SMS/command logic to a concrete transport.
