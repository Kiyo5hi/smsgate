---
status: proposed
created: 2026-04-09
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
`parseCmgrBody()` and `decodeUCS2()` together handle the text-mode
output. `decodeUCS2` has an `isHexString()` guard so plain ASCII passes
through unchanged — a workaround, not a solution.

## Plan

1. Switch to `AT+CMGF=0` (PDU mode).
2. Write a PDU decoder that handles:
   - GSM 7-bit default alphabet (with packing/unpacking)
   - 8-bit data
   - UCS2 / UTF-16BE
   - User Data Header (UDH) with concatenation IEI 0x00 (8-bit ref) and
     0x08 (16-bit ref)
3. Buffer in-flight long messages keyed by `(sender, ref)` with a TTL
   (24h?) and a max-buffered-bytes cap so a malicious sender can't OOM
   the bridge.
4. Reassemble and forward only when all parts arrive. Forward partial
   on TTL expiry so the user at least sees something.

## Notes for handover

- Keep `isHexString` and `decodeUCS2` around as helpers — the PDU
  decoder needs UCS2 → UTF-8 too.
- Look at `examples/` of TinyGSM and Adafruit FONA for reference PDU
  decoders, but **do not** pull in a heavy library; the PDU format is
  small enough to implement directly in <300 LOC.
- Test plan: send (a) a 30-char ASCII message, (b) a 30-char Chinese
  message, (c) a 200-char Chinese message that splits into 3 parts,
  (d) a flash SMS if you can find a sender that supports it.
