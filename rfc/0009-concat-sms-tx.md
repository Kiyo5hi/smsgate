---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0009: Concatenated (multipart) outbound SMS

## Motivation

The bridge can already receive and reassemble concatenated SMS (RFC-0002),
but outbound SMS is hard-capped at a single PDU: 160 GSM-7 chars or 70
UCS-2 chars. If a Telegram reply exceeds that limit,
`buildSmsSubmitPdu` returns false and `SmsSender::send` gives the user
an error like "SMS too long (max 160 chars for GSM-7)". This is
explicitly listed in CLAUDE.md as something that does not work yet.

Telegram messages routinely exceed 160 characters. Without concat TX,
users must manually truncate every reply, which makes the bridge
impractical for any conversation beyond one-liners. This is the single
biggest functional gap remaining in the TG -> SMS path.

## Current state

### `buildSmsSubmitPdu` (sms_codec.cpp, line 963)

Today the function:

1. Converts the UTF-8 body to Unicode code points via `utf8ToCodePoints`.
2. Attempts GSM-7 encoding: iterates code points through `encodeGsm7Char`,
   accumulating septets. If any code point has no GSM-7 representation,
   falls through to UCS-2.
3. **GSM-7 path**: rejects if `gsm7Septets.size() > 160`. Builds a
   single PDU with first octet `0x01` (SMS-SUBMIT, no VP, no UDHI),
   TP-DCS `0x00`, TP-UDL = septet count. Packs septets via
   `packSeptets` (no fill-bit offset support).
4. **UCS-2 path**: encodes code points to UTF-16BE via
   `codePointsToUtf16BE`. Rejects if `ucs2.size() > 140` (140 octets =
   70 BMP chars or fewer with surrogate pairs). Builds a single PDU with
   first octet `0x01`, TP-DCS `0x08`, TP-UDL = octet count.

Both paths emit: `[SCA=00] [01] [MR=00] [TP-DA...] [PID=00] [DCS] [UDL] [UD]`.

The return type is `bool`, with the PDU written into a single
`SmsSubmitPdu& out`.

### `packSeptets` (sms_codec.cpp, line 865)

Packs an array of 7-bit septets into bytes starting at bit offset 0.
Does **not** accept a `bitOffset` parameter. For concat GSM-7 parts,
where a 6-byte UDH must precede the packed septets, the packing must
start at a non-zero fill-bit offset so the first septet lands on a
7-bit boundary after the UDH. The existing function cannot do this.

(The RX-side `unpackSeptets` already handles arbitrary `bitOffset` --
the TX packer needs matching capability.)

### `SmsSender::send` (sms_sender.cpp)

Calls `buildSmsSubmitPdu` once. If it returns false, reports the length
error to the user via `lastError_`. If it returns true, sends the
single PDU via `modem_.sendPduSms`. No loop, no multi-part awareness.

### `IModem::sendPduSms`

Issues `AT+CMGS=<tpduLen>`, waits for the `>` prompt, writes PDU hex +
Ctrl-Z, waits up to 60s for OK. This interface is per-PDU and does not
need to change -- the caller (SmsSender) will just call it in a loop.

## Plan

### 1. Splitting logic

Introduce a helper (internal to `sms_codec`, in the anonymous namespace)
that splits a sequence of code points into parts that fit within the
per-part payload limits imposed by the 6-byte concat UDH:

| Encoding | UDH overhead | Payload per part       | Max single-part |
|----------|-------------|------------------------|-----------------|
| GSM-7    | 7 septets*  | 153 septets            | 160 septets     |
| UCS-2    | 6 octets    | 134 octets (67 chars)  | 140 octets      |

*The 6-byte UDH (UDHL=05, IEI=00, IEI-len=03, ref, total, seq)
occupies 6 octets = 48 bits. The next septet must start on a 7-bit
boundary, so we need `(7 - (48 % 7)) % 7 = (7 - 6) % 7 = 1` fill bit.
Total header = 49 bits = 7 septets exactly. 160 - 7 = 153 septets of
payload per part.

**GSM-7 splitting rule**: Walk the septet array and cut at 153-septet
boundaries. Extension-table characters (e.g. `[`, `{`, euro sign) are
2 septets each; a split must never land between the ESC (0x1B) and the
following septet. If the 153rd septet would be an ESC, end the current
part at 152 septets instead.

**UCS-2 splitting rule**: Walk the UTF-16BE octet array and cut at
134-octet (67-char) boundaries. A split must never land between the
high and low surrogate of a supplementary pair. If the 67th code unit
would be a high surrogate (0xD800-0xDBFF), end the current part at 66
code units (132 octets) instead.

**If the body fits in a single PDU** (<=160 GSM-7 septets or <=140
UCS-2 octets), produce a single-element vector with NO UDH -- identical
to today's output. This avoids wasting the 7-septet / 6-octet UDH
overhead and ensures backward compatibility with carriers that might
handle single-PDU messages differently from concat.

### 2. UDH construction

Each part gets a Concat SMS UDH using IEI 0x00 (8-bit reference number):

```
UDHL = 0x05         (5 bytes of IEs follow)
IEI  = 0x00         (concatenation, 8-bit ref)
IEDL = 0x03         (3 bytes of IE data)
Ref  = <ref_number> (8-bit, shared across all parts)
Total = <total>     (total part count, 1-based)
Seq   = <seq>       (this part's sequence number, 1-based)
```

Total UDH: 6 bytes.

The reference number should be chosen pseudo-randomly or as a
monotonically increasing counter (mod 256). An `static uint8_t` counter
in `buildConcatSmsSubmitPdus` suffices -- it does not need to be
persistent across reboots because the ref only needs to be unique among
messages currently in-flight to the same recipient, and the chance of
collision with a simple wrapping counter is negligible for our use case.

IEI 0x08 (16-bit ref) is not needed for TX: 8-bit gives 256 distinct
references, which is more than enough. (The RX side already handles
both 0x00 and 0x08 for interoperability with senders that use 16-bit
refs.)

### 3. PDU construction changes

Today, the first octet of the TPDU is `0x01` (TP-MTI=01 SMS-SUBMIT,
all other bits zero). For concat parts, bit 6 (TP-UDHI) must be set
to indicate that the UD field starts with a UDH:

```
0x01 | 0x40 = 0x41   (SMS-SUBMIT + UDHI)
```

The rest of the PDU envelope (SCA, TP-MR, TP-DA, TP-PID, TP-DCS) is
identical across parts.

**TP-UDL** for each part:

- **GSM-7**: UDL counts *septets* for the entire UD field, including the
  UDH expressed in septets. UDL = 7 (header septets) + payload septets
  for this part. For example, a full 153-septet part has UDL = 160.

- **UCS-2**: UDL counts *octets* for the entire UD field, including the
  UDH. UDL = 6 (header octets) + payload octets for this part. For
  example, a full 134-octet part has UDL = 140.

**UD field layout for GSM-7 with UDH**:

```
[UDHL=05] [IEI=00] [IEDL=03] [ref] [total] [seq] [fill] [packed septets...]
```

The 6 UDH bytes occupy 48 bits. The next septet must be aligned to a
7-bit boundary, so 1 fill bit (zero) is inserted. The packed septets
then start at bit offset 49. `packSeptets` must be extended to accept a
`bitOffset` parameter (see section 4).

**UD field layout for UCS-2 with UDH**:

```
[UDHL=05] [IEI=00] [IEDL=03] [ref] [total] [seq] [UTF-16BE octets...]
```

No fill bits needed -- UCS-2 is byte-aligned. The UDH is simply
prepended.

### 4. `packSeptets` enhancement

Add an optional `size_t bitOffset` parameter (defaulting to 0 for
backward compatibility):

```cpp
std::vector<uint8_t> packSeptets(const std::vector<uint8_t> &septets,
                                 size_t bitOffset = 0);
```

The packing loop changes from:

```cpp
size_t bitPos = i * 7;
```

to:

```cpp
size_t bitPos = bitOffset + i * 7;
```

And `numBytes` becomes:

```cpp
size_t numBytes = (bitOffset + septets.size() * 7 + 7) / 8;
```

This mirrors how `unpackSeptets` already handles arbitrary bit offsets
on the RX side. Existing call sites pass `bitOffset = 0` (or omit it)
and behave identically.

### 5. API surface changes

#### `sms_codec.h`

Replace the current single-PDU signature:

```cpp
bool buildSmsSubmitPdu(const String &phone, const String &body,
                       SmsSubmitPdu &out);
```

with a multi-PDU signature:

```cpp
bool buildSmsSubmitPdu(const String &phone, const String &body,
                       std::vector<SmsSubmitPdu> &out,
                       size_t maxParts = 10);
```

- Returns false if the body exceeds `maxParts` parts (safety cap).
- Returns false if `phone` or `body` is empty.
- On success, `out` contains 1..N `SmsSubmitPdu` structs. If the body
  fits in a single non-concatenated SMS, `out` has exactly 1 element
  with no UDH (identical to today's output).
- The old single-PDU overload should be kept as a thin wrapper that
  calls the vector version and returns `out[0]` on success, returning
  false if the result has more than 1 part. This preserves any test
  code that uses the old signature without modification.

#### `SmsSender::send`

Changes from:

```cpp
sms_codec::SmsSubmitPdu pdu;
if (!sms_codec::buildSmsSubmitPdu(number, body, pdu))
    ...
bool ok = modem_.sendPduSms(pdu.hex, pdu.tpduLen);
```

to:

```cpp
std::vector<sms_codec::SmsSubmitPdu> pdus;
if (!sms_codec::buildSmsSubmitPdu(number, body, pdus))
    ...
for (size_t i = 0; i < pdus.size(); ++i) {
    bool ok = modem_.sendPduSms(pdus[i].hex, pdus[i].tpduLen);
    if (!ok) {
        lastError_ = "modem rejected part " + String((int)(i + 1))
                   + " of " + String((int)pdus.size());
        return false;
    }
}
return true;
```

No inter-part delay is added. The A7670X modem accepts back-to-back
`AT+CMGS` commands as long as each one completes (OK response) before
the next is issued, which the synchronous `sendPduSms` already
guarantees. If field testing reveals that some networks need a short
delay between parts, a `delay(500)` can be added later -- but do not
add speculative delays now.

### 6. Error handling

**Partial delivery**: if part N of M fails, `SmsSender::send` returns
false immediately and sets `lastError_` to indicate which part failed.
The recipient will have received parts 1..N-1 but not N..M. The user
sees the error in Telegram and can retry the full message.

No automatic retry is implemented. Rationale:

- The modem's `AT+CMGS` failure is typically a radio/network issue (no
  signal, congestion). Retrying immediately is unlikely to help and
  risks sending duplicate fragments that confuse the reassembly on the
  recipient's phone.
- The failure counter in `SmsHandler` (which triggers reboot after 8
  consecutive Telegram POST failures) is for the *inbound* path and
  does not apply here. Outbound SMS failures do not increment it.

**Already-delivered parts are not recalled**. SMS has no recall
mechanism. The user must accept that partial delivery is possible,
just as it is with any multi-part SMS sender. This matches how every
phone on the market behaves.

### 7. Maximum message length cap

Default `maxParts = 10`, giving:

- GSM-7: 10 x 153 = 1,530 characters
- UCS-2: 10 x 67 = 670 characters

This is generous enough for any normal Telegram reply while preventing
a single message from consuming excessive airtime/cost. The cap is a
parameter, so it can be adjusted via a `#define` or build flag if
needed.

If the body exceeds `maxParts` parts, `buildSmsSubmitPdu` returns false
and `SmsSender` reports "SMS too long (max ~1530 chars for GSM-7)" or
"SMS too long (max ~670 chars for Unicode)" as appropriate.

### 8. Test plan

All new logic is exercised in the native test env (`pio test -e native`)
with no hardware dependency:

1. **`packSeptets` with bitOffset**: round-trip test encoding N septets
   at bitOffset=49 (the concat UDH case), then decoding with
   `unpackSeptets` at the same offset. Verify output matches input.

2. **Single-part backward compatibility**: verify that a body of <=160
   GSM-7 / <=140 UCS-2 octets produces a 1-element vector with no UDH,
   and that the PDU hex is byte-identical to what the old single-PDU
   function produced.

3. **Two-part GSM-7**: a 161-character ASCII body. Verify 2 PDUs are
   emitted, first octet is `0x41` (UDHI set), UDH bytes are correct,
   UDL of part 1 is 160 (7 header septets + 153 payload), part 2's
   UDL is 7 + 8 = 15 (7 header septets + 8 remaining). Decode both
   PDUs with `parseSmsPdu` and verify the reassembled text matches the
   original.

4. **Two-part UCS-2**: a 71-character Chinese string. Verify 2 PDUs,
   UDH correct, payload split at 67-char boundary, round-trip decodes
   correctly.

5. **ESC-safe split**: a body of exactly 153 chars where char 153 is
   `[` (extension table: ESC + 0x3C = 2 septets). Verify the split
   does not break the escape sequence -- part 1 should end at 152
   septets, pushing the `[` entirely into part 2.

6. **Surrogate-safe split**: a body containing supplementary Unicode
   (e.g. emoji) positioned at the 67th code-unit boundary. Verify the
   split does not land between surrogate pairs.

7. **Max-parts cap**: a body exceeding 10 parts. Verify
   `buildSmsSubmitPdu` returns false.

8. **SmsSender multi-part send**: wire a `FakeModem` that accepts the
   first N PDUs but rejects part N+1. Verify that `send` returns false
   with the correct `lastError_` string, and that parts 1..N were
   indeed submitted to the modem.

9. **Concat ref uniqueness**: call `buildSmsSubmitPdu` twice. Verify
   the two calls produce different concat reference numbers.

### 9. CLAUDE.md / capability updates

After implementation:

- Move "Long SMS replies exceeding 160 GSM-7 / 70 Unicode chars" from
  the "does not work yet" section to the "works" section.
- Update the SmsSender description in the architecture section to
  mention multi-part support and the 10-part cap.
- Update the TG -> SMS description to note that concat TX is supported.

## Notes for handover

- **The `packSeptets` bitOffset change is the trickiest part.** The
  fill bit(s) after the UDH must be zero-valued, and the packed
  septets must start at exactly `(UDHL+1)*8 + fillBits` bits into the
  UD octet stream. The existing RX-side `unpackSeptets` already handles
  this, so use it as the reference implementation. The packer should
  allocate its output buffer with the correct total bit count and leave
  the first `bitOffset` bits as zero (they overlap with the last byte
  of the UDH in the final PDU assembly, so the UDH bytes must be
  written first, then the packed septets overlaid starting from the
  byte that contains the fill bits).

- **PDU assembly order matters for GSM-7.** The UDH bytes and the
  packed septets share bytes when fill bits are involved. Concretely,
  with a 6-byte UDH and 1 fill bit, byte index 6 of the UD contains
  the fill bit in its MSB (bit 0 of the packed output) and the first
  septet starts at bit 1. The cleanest approach: (a) allocate the
  full UD as a byte array of `ceil((7 + payloadSeptets) * 7 / 8)`
  bytes zeroed, (b) write the 6 UDH bytes into positions 0-5, (c) call
  `packSeptets(septets, 49)` which returns bytes starting from byte 0
  of a fresh buffer where bit 49 is the first septet, (d) OR the
  packed bytes into UD bytes 6 onward (they won't collide because
  the first 48 bits of the packed buffer are zero and the fill bit at
  position 48 is also zero). Actually, the simpler approach: build the
  UDH as 6 bytes, call `packSeptets(septets, 49)` to get a buffer
  whose first 6 bytes are 0x00 (the 48 zero bits), then overwrite
  bytes 0-5 of that buffer with the UDH. This avoids any OR merging.

- **UCS-2 is simpler.** No bit-alignment issue. Just prepend the 6-byte
  UDH to the UTF-16BE payload octets. UDL = 6 + payload_octets.

- **Do not change `IModem::sendPduSms`.** The per-PDU interface is
  correct. The multi-part loop belongs in `SmsSender`.

- **The `build_src_filter` in `platformio.ini`** already includes
  `+<sms_codec.cpp>` and `+<sms_sender.cpp>` for the native env, so no
  build config changes are needed. The new test file
  `test_sms_pdu_encode.cpp` already exists and can be extended in place,
  or a new `test_concat_sms_tx.cpp` can be added -- either works since
  `test_main.cpp` calls `UNITY_BEGIN/END` and includes all test files
  via the PlatformIO test runner.

- **Existing single-PDU tests must not break.** Keep the old
  `bool buildSmsSubmitPdu(phone, body, SmsSubmitPdu&)` overload as a
  thin wrapper. All 11 existing tests in `test_sms_pdu_encode.cpp` call
  this overload and must continue to pass unchanged.

## Review

verdict: approved-with-changes

### Issues

- **BLOCKING — `packSeptets` is in an anonymous namespace and cannot be
  called directly from test code.** Test plan item 1 ("round-trip test
  encoding N septets at bitOffset=49") implies directly calling
  `packSeptets` from the Unity test file. That is impossible as long as
  the function lives in the second anonymous namespace in `sms_codec.cpp`.
  The RFC must either (a) promote `packSeptets` (with the new signature)
  to the public `sms_codec` namespace and declare it in `sms_codec.h`, so
  tests can call it directly, or (b) drop test item 1 as a standalone test
  and fold the round-trip coverage into the existing PDU-level tests (items
  2-4). Option (a) is cleaner because the bitOffset logic is subtle and
  deserves direct unit coverage; option (b) avoids growing the public API.
  Either way the RFC must resolve the ambiguity before implementation.

- **BLOCKING — "Notes for handover" contains an incorrect bit-position
  description that will mislead the implementer.** Section "PDU assembly
  order matters for GSM-7" says: "byte index 6 of the UD contains the fill
  bit in its **MSB** (bit 0 of the packed output)". This is wrong on two
  counts. With bitOffset=49: `byteIdx = 49/8 = 6`, `bitOff = 49%8 = 1`.
  The fill bit lives at bit 0 (the **LSB**, not MSB) of byte 6. The first
  septet starts at bit 1 (the second-least-significant bit) of byte 6. The
  sentence needs to be corrected to "fill bit in its **LSB** (bit 0 of byte
  6)" to avoid an off-by-one error when the implementer writes the assembly
  code. The description of the overwrite approach is otherwise sound.

- **NON-BLOCKING — `numBytes` formula in the proposed `packSeptets` change
  produces an off-by-zero-for-the-right-reason situation that should be
  explicitly called out.** `numBytes = (bitOffset + septets.size()*7 + 7)/8`
  uses integer (truncating) division which happens to equal `ceil(...)` for
  all valid inputs because the `+7` ensures rounding up. This is the same
  idiom as the current code at bitOffset=0 and is correct. However, the
  existing guard `if (byteIdx + 1 < numBytes)` is not updated in the RFC.
  With the new `numBytes` formula, for septet i=0 at bitOffset=49
  (`bitPos=49`, `byteIdx=6`, `bitOff=1`), the high-byte write target is
  `out[7]`. Because the 7-bit septet shifted left 1 never produces a
  non-zero high byte, the guard trivially fires (7 < 140) and writes 0x00 --
  harmless but worth a comment. For the general case the guard logic is
  unchanged and correct. The RFC should explicitly state the guard is
  unchanged to close the loop.

- **NON-BLOCKING — The old single-PDU wrapper's failure semantics differ
  from the current function's.** Today `buildSmsSubmitPdu(phone, body,
  SmsSubmitPdu&)` returns false when the body is too long (> 160 GSM-7
  septets or > 140 UCS-2 octets). The proposed wrapper returns false if
  the result has more than 1 part -- but the vector variant accepts up to
  `maxParts` (default 10) parts before returning false. This means the
  wrapper's false-return now fires at 2+ parts instead of at the >160/140
  threshold. Any caller of the old overload that passes a 161-char body
  expecting false will now get... the wrapper returning false because the
  vector has 2 parts. The observable result is the same (false is returned)
  but the reason is different, and `SmsSender`'s error-path branches on
  `isGsm7Compatible()` after the false return to set `lastError_`. After
  this change `SmsSender` will need updating to not call the old overload
  at all (which it won't -- it calls the vector variant). The old overload
  exists purely for the 11 legacy tests. The RFC should state that
  `SmsSender::send` will be updated to call the vector variant directly and
  that the thin wrapper is test-only scaffolding, not a production call
  site.

- **NON-BLOCKING — Test plan is missing an explicit "exactly at the
  single-part boundary" case.** Item 2 verifies <=160 GSM-7 produces 1
  part; item 3 uses 161 chars. But there is no test for exactly 160 GSM-7
  chars (produces 1 part, no UDH) versus 161 chars (produces 2 parts with
  UDH). Add a test that builds a 160-char body and asserts: vector size=1,
  first-octet=0x01 (no UDHI), then builds a 161-char body and asserts:
  vector size=2, first-octet=0x41 (UDHI set). The analogous boundary for
  UCS-2 is 70 chars (1 part) vs 71 chars (2 parts). These are the most
  likely regression points.

- **NON-BLOCKING — Test plan item 9 (ref uniqueness) is under-specified.**
  "Call `buildSmsSubmitPdu` twice. Verify different concat reference
  numbers." This only tests that the counter increments once. A more
  valuable test: call it N times, collect all ref bytes, verify all N are
  distinct mod 256 (or at least that consecutive calls differ). Also:
  single-part messages (no UDH) should not increment the counter, since
  they carry no concat ref. Clarify whether the counter increments on
  every call or only when the body actually requires multi-part splitting.

- **NON-BLOCKING — ESC-safe split rule (section 1) needs clarification on
  the case where `septets[152]` is the second byte of an ESC sequence
  (i.e., `septets[151]` is 0x1B).** The RFC only says "if the 153rd septet
  would be an ESC, end the current part at 152." But an ESC at position 151
  (0-indexed) means the extension character is at 152 -- the split would
  land after the extension character, which is fine. The problematic case
  is `septets[151] == 0x1B`: you cannot end the part at 152 because that
  leaves a lone ESC as the last septet of part 1 (which decoders will
  ignore or misinterpret). The split rule should be: if `septets[152]` is
  0x1B **or** `septets[151]` is 0x1B, end the part at 151 septets. Without
  this fix the ESC-safe split test (item 5) does not cover the
  lone-ESC-at-end-of-part failure mode.

### Summary

The RFC is technically sound in its core 3GPP arithmetic: UDH byte layout,
fill-bit calculation (1 fill bit after a 6-byte UDH), GSM-7 per-part capacity
(153 septets), UCS-2 per-part capacity (67 code units), and UDL accounting (7
header septets + payload) are all correct. The `packSeptets` bitOffset
extension is the right approach and mirrors the existing RX-side
`unpackSeptets`. Two issues must be resolved before implementation starts: the
anonymous-namespace visibility problem that makes the stated test plan
unexecutable as written, and a factual error in the handover notes that
inverts MSB/LSB in the fill-bit description. The four non-blocking items
(wrapper semantics, boundary test gap, ref-counter increment scope, and the
lone-ESC edge case in the split rule) should be addressed before the PR lands
to avoid latent bugs and test gaps, but they do not block the design from being
correct in the common case.

## Code Review

verdict: approved

### Issues

**1. bitOffset=49 arithmetic — CORRECT, no issue**

The question was whether bitOffset should be 48 or 49. The math: UDH is 6 bytes
= 48 bits. Fill bits needed = `(7 - (48 % 7)) % 7 = (7 - 6) % 7 = 1`. First
user septet starts at bit 48 + 1 = **49**. `packSeptets(chunk, 49)` is correct.
With `bo=49`, septet 0 lands at `bitPos=49`, `byteIdx=6`, `bitOff=1`. Bytes 0-5
of the packed buffer are zero (no septet has a bitPos in [0..47]); the
implementation then overwrites bytes 0-5 with the UDH bytes. Byte 6 bit 0 is
the fill bit (zero, never written by any septet), and bits 1-7 of byte 6 carry
the low 6 bits of septet 0 plus the start of septet 1 overflow. This exactly
matches the 3GPP packing convention.

**2. UDL field for GSM-7 concat — CORRECT, no issue**

`sms_codec.cpp` line 1092: `uint8_t udl = (uint8_t)(7 + chunk.size())`. The
6-byte UDH occupies 48 bits = 6 full septets + 1 fill-bit septet = 7 septets.
UDL = 7 + payload septets. For a full 153-septet part: UDL = 160 (verified by
`test_buildSmsSubmitPduMulti_161_gsm7_two_parts` which asserts
`hexByte(pdus[0].hex, 16) == 160`). Correct per 3GPP TS 23.040 §9.2.3.16.

**3. ESC-safe split — CORRECT, both checks present**

`sms_codec.cpp` lines 1051-1054:
```cpp
if (gsm7Septets[pos + 152] == 0x1B)
    chunkSize = 152;
else if (gsm7Septets[pos + 151] == 0x1B)
    chunkSize = 151;
```
The implementation checks both the 153rd septet (index 152, 0-based) being ESC
— which would orphan the extension char in the next part — and the 152nd septet
(index 151) being ESC — which would leave a lone ESC as the last septet of the
current part. The `else if` ordering is correct: if index 152 is ESC, trim to
152 (the ESC and its extension char both move to the next part); if index 151 is
ESC, trim to 151 (the lone ESC cannot end a part). This resolves the NON-BLOCKING
concern raised in the RFC Review section and is more conservative than the RFC
originally specified.

**4. Surrogate-safe UCS-2 split — CORRECT, no issue**

`sms_codec.cpp` lines 1152-1156: reads bytes at `pos+132` and `pos+133` (the
67th UTF-16 code unit, 0-indexed as code unit 66), checks whether it is a high
surrogate (0xD800-0xDBFF), and trims to 132 bytes (66 code units) if so. The
split point check is at the correct position (the last code unit that would be
included, not the first of the next part).

**5. Single-part path — CORRECT, re-implemented inline but byte-identical**

`buildSmsSubmitPduMulti` re-implements the single-part path rather than calling
the old `buildSmsSubmitPdu`. The output is structurally identical: first octet
`0x01` (no UDHI), no UDH bytes. The backward-compat wrapper
`buildSmsSubmitPdu(phone, body, SmsSubmitPdu&)` delegates to
`buildSmsSubmitPduMulti` and returns false if `pdus.size() != 1`, so any caller
that previously got `false` for a body > 160 chars still gets `false` (the
vector has 2+ parts). The `test_buildSmsSubmitPdu_gsm7_max_160` test
(line 171) continues to verify this.

**6. buildSmsSubmitPdu backward-compat wrapper — NON-BLOCKING, acceptable**

The wrapper (`sms_codec.cpp` lines 1210-1218) returns false when
`pdus.empty() || pdus.size() > 1`. For a 161-char body, `buildSmsSubmitPduMulti`
returns 2 PDUs → false. Old callers that passed a 161-char body got false before
(body too long) and still get false (too many parts). Observable behavior is
unchanged. `SmsSender::send` has been updated to call `buildSmsSubmitPduMulti`
directly and no longer uses this wrapper at all. The wrapper is retained for the
11 legacy test cases only, as intended. This is acceptable.

**7. Static ref counters — CORRECT, function-local statics at sms_codec namespace scope**

`s_concatRef` (line 1032) is declared inside the multi-part GSM-7 branch of
`buildSmsSubmitPduMulti`. `s_concatRefUcs2` (line 1138) is declared in the
multi-part UCS-2 branch. Both are function-local statics: initialized once on
first execution of the enclosing branch, then persist for program lifetime.
Neither branch is reachable on a single-part call (GSM-7 branch requires
`gsm7Septets.size() > 160`; UCS-2 branch requires `ucs2.size() > 140`), so
single-part messages do not consume reference numbers. The
`test_buildSmsSubmitPduMulti_ref_uniqueness` test (line 444) explicitly
verifies this property by making two single-part calls before the multi-part
calls and asserting the two multi-part refs differ.

**8. Test coverage of UDH byte structure — ADEQUATE**

`test_buildSmsSubmitPduMulti_161_gsm7_two_parts` (line 315) verifies at the
byte level:
- UDHL = 0x05 at hex offset 18 of part 1 PDU
- IEI = 0x00 at hex offset 20
- IEDL = 0x03 at hex offset 22
- total = 2 at hex offset 26
- seq = 1 at hex offset 28 (part 1), seq = 2 at hex offset 28 (part 2)
- ref byte identical across both parts

This is genuine byte-level UDH verification, not merely a split-count check.
UCS-2 UDH bytes are not independently byte-verified (only part count and UDL
are checked in `test_buildSmsSubmitPduMulti_71_ucs2_two_parts`), but the
structural code paths are symmetric and the GSM-7 coverage is adequate for
blocking regressions.

**9. Additional issue found — NON-BLOCKING: `test_SmsSender_partial_failure_second_part` asserts 2 modem calls but documents "Both parts were attempted"**

`test_sms_sender.cpp` line 219: `TEST_ASSERT_EQUAL(2, (int)modem.pduSendCalls().size())`.
The FakeModem queues `true` then `false`. `SmsSender::send` calls `sendPduSms`
for index 0 (gets `true`, continues), then index 1 (gets `false`, returns false
immediately). The modem records both calls regardless of return value. The
assertion is correct and the test will pass. No bug.

**10. Additional issue found — NON-BLOCKING: `packSeptets` buffer-overwrite safety for chunks of exactly 1 septet**

With `bitOffset=49` and 1 septet, `numBytes = (49 + 7 + 7) / 8 = 63 / 8 = 7`
(integer division). Indices 0-5 are valid and zeroed. The overwrite at lines
1082-1087 writes indices 0-5, which is safe. There is no off-by-one here.

### Summary

All seven items in the review focus are confirmed correct. The two BLOCKING
issues raised in the RFC's own `## Review` section (anonymous-namespace
visibility of `packSeptets`, and the MSB/LSB handover note error) have both
been resolved in the implementation: `packSeptets` is now declared in
`sms_codec.h` and implemented in the `sms_codec` namespace (not an anonymous
namespace), and the fill bit correctly lands at byte 6 bit 0 (LSB) as the
actual packing math requires. The four NON-BLOCKING items from that review
section have also been addressed: `SmsSender` calls `buildSmsSubmitPduMulti`
directly; boundary tests for exactly 160/70 chars exist; the ref counter
increments only on multi-part messages; and the ESC split checks both
`septets[152]` and `septets[151]`. The implementation is arithmetically
correct, the tests exercise the byte-level UDH structure and the single/multi
boundary, and there are no blocking issues remaining. This PR is approved as-is.
