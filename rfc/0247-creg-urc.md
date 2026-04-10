---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0247: Real-time registration status via +CREG URC

## Motivation

Before this change, network registration changes are only detected in the
30 s status-refresh block (`AT+CREG?` polling).  This means:

- A registration-lost alert (RFC-0082) can fire up to 30 s late.
- A registration-restored alert can fire up to 30 s late.
- The `cachedRegStatus` value used by `/status` is up to 30 s stale.

The A76xx supports spontaneous `+CREG: <stat>` URCs triggered by the modem
itself as soon as registration changes, by enabling them with `AT+CREG=1`.

## Plan

1. **`setup()`**: send `AT+CREG=1` immediately after `+CLIP=1` so the modem
   starts emitting unsolicited `+CREG: <stat>` URCs at boot.

2. **Three re-arm locations** (RFC-0242 periodic sweep, RFC-0242 health check,
   RFC-0245 modem soft-reset handler): add `AT+CREG=1` after `+CLIP=1` so
   the subscription is restored if the modem clears it during normal operation.

3. **URC drain loop**: add a `+CREG:` handler between the RFC-0245 block and
   the `callHandler.onUrcLine()` call.  The handler:
   - Parses the stat field.  Handles both `+CREG: <stat>` (URC format) and
     `+CREG: <n>,<stat>` (solicited-response format, can appear via RFC-0236
     piggybacked-URC dispatch).
   - Updates `cachedRegStatus` immediately.
   - Fires the RFC-0082 reg-lost / reg-restored alert inline (same logic as
     the 30 s block).  Idempotent: `s_regLostAlertSent` guards double-fire.

## Notes for handover

The solicited `AT+CREG?` response with `n=1` active is `+CREG: 1,<stat>`.
The existing 30 s block parser already handles this correctly (looks for
first comma, takes everything after it).  No change needed there.

The RFC-0236 piggybacked-URC dispatch can re-process a `+CREG: 1,<stat>`
line from s236raw after the 30 s block has already parsed it.  The drain-loop
handler fires a second time, but since `cachedRegStatus` and `s_regLostAlertSent`
are already in the correct state, the RFC-0082 check is a no-op.
