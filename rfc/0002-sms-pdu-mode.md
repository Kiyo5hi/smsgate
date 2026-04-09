---
status: proposed
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0002: Switch SMS receive to PDU mode

## Motivation

We currently use SMS text mode (`AT+CMGF=1`) with `AT+CSCS="UCS2"`. This
works for the common case (Chinese verification codes from short codes)
but has real limitations:

- **No long-SMS reassembly.** Anything over 70 UCS2 chars / 160 GSM-7
  chars is split by the sender into multiple PDUs with a User Data Header
  carrying a reference id, total parts, and part index. In text mode the
  module hands us each part as an independent message and the UDH info
  is lost. We forward fragments out of order with no way to stitch them.
- **Encoding ambiguity.** Some operators send GSM-7 packed; some send
  8-bit; some send UCS2. In text mode the response shape changes per
  encoding, and our `decodeUCS2` + `isHexString` guard is a fragile
  heuristic.
- **No delivery class info.** Flash SMS, silent SMS, MWI indicators —
  all distinguishable from PDU but invisible in text mode.

## Current state

`src/main.cpp` `setup()`:
```
modem.sendAT("+CMGF=1");
modem.sendAT("+CSCS=\"UCS2\"");
```
`parseCmgrBody()` and `decodeUCS2()` live in `src/sms.cpp` and together
handle the text-mode output. `decodeUCS2` has an `isHexString()` guard
so plain ASCII passes through unchanged — a workaround, not a solution.

A76xx TinyGSM does not abstract `CMGF` — it flips the module into text
mode before every send, and the specific command sequence depends on
which send API is called:

- `sendSMSImpl` at `TinyGSM/src/TinyGsmSMS.tpp:139` (the ASCII path
  used by `modem.sendSMS(number, text)`) sends **both** `+CMGF=1` AND
  `+CSCS="GSM"` before each message (see lines 141-145).
- `sendSMS_UTF16Impl` (the UCS2 path used by
  `modem.sendSMS_UTF16(number, buf, len)`) goes through
  `sendSMS_UTF8_begin` at line 185, which sends `+CMGF=1`,
  `+CSCS="HEX"`, and `+CSMP=17,167,0,8` before the body.

So: switching our *receive* path to PDU mode does not break sends —
TinyGSM will flip the module back into text mode (and set its own CSCS)
on every send, regardless of what we set up in our own configure path.
Our URC-driven receive loop just needs to be tolerant of the
mode-flipping, which it already is (we re-read `AT+CNMI`-triggered
indices via `AT+CMGR=<idx>` and parse whatever mode the module is
currently in).

## Plan

**Scope.** This RFC is **receive-side only**. TinyGSM continues to
drive the send path via its existing `sendSMS` / `sendSMS_UTF16` calls
(which flip the module back into text mode with their own CSCS, as
documented in "Current state"). Send-side encoding is RFC-0003's
concern, not this RFC's.

1. Switch the receive-side setup in `main.cpp` / (post-0007) the
   `SmsHandler` constructor to `AT+CMGF=0` (PDU mode). Drop the
   `+CSCS="UCS2"` line — it's only meaningful in text mode.

   **CSCS is irrelevant in PDU mode.** `+CMGR` in PDU mode emits hex
   PDU bytes regardless of the current CSCS setting, so the
   timing-dependent flip-flop between `+CSCS="GSM"` (left behind by a
   TinyGSM ASCII send), `+CSCS="HEX"` (left behind by a TinyGSM UCS2
   send), and whatever else touches the module does **not** affect
   receive-side correctness. We can ignore CSCS entirely on the
   receive path once PDU mode is on.

2. Write a PDU decoder that handles:
   - GSM 7-bit default alphabet (with packing/unpacking)
   - 8-bit data
   - UCS2 / UTF-16BE
   - User Data Header (UDH) with concatenation IEI 0x00 (8-bit ref) and
     0x08 (16-bit ref)
3. Buffer in-flight long messages keyed by `(sender, ref)` with the
   following concrete caps so a malicious sender cannot OOM the bridge:

   - **TTL per key: 24 hours**, measured from the first part received
     for that `(sender, ref)` pair. On TTL expiry, forward whatever
     fragments arrived as a single Telegram message clearly marked
     `[partial: N/M parts]` and then drop the key.
   - **Max concurrent keys: 8.** If a 9th `(sender, ref)` pair arrives
     with no room, evict the oldest key (LRU) — forward whatever it
     had as `[partial: N/M parts]` before dropping — and log the
     eviction.
   - **Max bytes per key: 2 KB.** Enough for ~10 × 160-byte fragments.
     If a key would exceed this, drop the key (LRU-evict it the same
     way: forward partial, log) and do not accept the new fragment.
   - **Max bytes total across all keys: 8 KB.** Same eviction policy
     on overflow.

4. Reassemble and forward only when all parts arrive. Forward partial
   on TTL expiry or LRU eviction (see caps above) so the user at least
   sees something, clearly marked as partial.

## Notes for handover

- **Architecture.** This RFC must land *after* RFC-0007 (testability)
  so the PDU decoder can be developed host-side with native unit tests
  against canned PDU hex strings from the wild. The new decoder lives
  as pure functions in `src/sms_codec.{h,cpp}` (alongside the existing
  `decodeUCS2` / `isHexString` helpers 0007 extracts there) and is
  invoked from `SmsHandler::handleSmsIndex`. No new interface is
  needed — the existing `IModem` seam already carries whatever the
  module returns from `AT+CMGR`, which in PDU mode is a hex blob
  instead of a structured text record.
- Keep `isHexString` and `decodeUCS2` around as helpers — the PDU
  decoder needs UCS2 → UTF-8 too.
- Look at `examples/` of TinyGSM and Adafruit FONA for reference PDU
  decoders, but **do not** pull in a heavy library; the PDU format is
  small enough to implement directly in <300 LOC.
- **AT command verification.** `AT+CMGF=0` and `AT+CMGR=<idx>` returning
  a PDU hex string are standard 3GPP TS 27.005 commands and supported
  by A76xx. `+CNMI=2,1,0,0,0` works identically in both text and PDU
  modes (the URC just carries the index; the stored PDU is what
  differs).
- Test plan: send (a) a 30-char ASCII message, (b) a 30-char Chinese
  message, (c) a 200-char Chinese message that splits into 3 parts.
  Unit tests (via RFC-0007) should additionally cover: UDH 8-bit ref,
  UDH 16-bit ref, out-of-order part arrival, partial TTL expiry,
  LRU eviction under cap pressure, ref-id collision across distinct
  senders. Flash SMS / silent SMS / MWI indicators are future work —
  basically impossible to provoke in the wild and not worth gating
  this RFC on.
