---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0011: SMS delivery report notifications

## Motivation

When a user replies to a forwarded SMS in Telegram, the TG -> SMS path
(RFC-0003) currently posts a confirmation like
"Reply sent to +86 138-0000-1234". But "sent" only means the modem
accepted the SMS-SUBMIT PDU and returned OK to `AT+CMGS` -- it does
**not** mean the recipient's handset received the message. The SMS could
be sitting in the SMSC queue, could have been rejected by the
destination network, or could be undeliverable because the recipient's
phone is off. The user has no way to tell.

The 3GPP SMS standard (TS 23.040 / TS 23.038) supports **Status
Reports**: the sender sets the TP-SRR (Status Report Request) bit in the
SMS-SUBMIT PDU, and the SMSC delivers back an SMS-STATUS-REPORT PDU when
it has a final disposition. The A76xx modem family supports this
mechanism and can route status reports to the TE via `+CDS` (unsolicited
full PDU) or `+CDSI` (unsolicited storage index, like `+CMTI` for normal
SMS).

This RFC adds delivery report support so the user sees:

- "Delivered to +86 138-0000-1234" when the recipient's phone acked, or
- "Delivery failed to +86 138-0000-1234: <reason>" when the SMSC reports
  a permanent or temporary failure.

## Dependencies

- **Hard: RFC-0003 (bidirectional TG -> SMS) must be `implemented`.**
  Without outbound SMS there is nothing to report on. (Already done.)
- **Hard: RFC-0007 (testability) must be `implemented`.** The new
  status-report parser, correlation buffer, and notification logic
  should all be host-testable against canned hex PDUs and fake
  collaborators. (Already done.)
- **Soft: RFC-0002 (PDU mode).** We are already in PDU mode, which is
  required to receive and parse SMS-STATUS-REPORT PDUs. (Already done.)

## Current state

### SMS-SUBMIT PDU construction (`sms_codec.cpp`, lines 963-1020)

`buildSmsSubmitPdu` constructs the SMS-SUBMIT first octet as `0x01`:

```
pdu.push_back(0x01); // SMS-SUBMIT, no VP, no UDHI, no SRR
pdu.push_back(0x00); // TP-MR: modem assigns
```

Bit layout of `0x01`:

| Bit | Field   | Value | Meaning               |
|-----|---------|-------|-----------------------|
| 0-1 | TP-MTI | 01    | SMS-SUBMIT            |
| 2   | TP-RD  | 0     | accept duplicates     |
| 3-4 | TP-VPF | 00    | no Validity Period     |
| 5   | TP-SRR | 0     | **no status report**  |
| 6   | TP-UDHI| 0     | no UDH                |
| 7   | TP-RP  | 0     | no reply path         |

TP-MR (Message Reference) is set to `0x00`, meaning the modem assigns
the reference number from its internal counter. The modem echoes it back
in the `+CMGS: <mr>` response line after accepting the PDU, but
`RealModem::sendPduSms` (`real_modem.h`, line 58) currently discards it:

```cpp
return modem_.waitResponse(60000UL) == 1;  // only checks OK vs ERROR
```

### Outbound flow (`sms_sender.cpp`)

`SmsSender::send` calls `modem_.sendPduSms(pdu.hex, pdu.tpduLen)` and
returns a bool. No message reference is captured, so there is nothing to
correlate a future status report against.

### URC dispatch (`main.cpp`, loop lines 337-360)

The main loop drains `SerialAT` line by line and dispatches:

- `+CMTI:` -> `smsHandler.handleSmsIndex(idx)`
- Everything -> `callHandler.onUrcLine(line)`

There is no handler for `+CDS:` or `+CDSI:` URCs.

### `AT+CNMI` configuration (`main.cpp`, line 284)

```cpp
modem.sendAT("+CNMI=2,1,0,0,0");
//                       ^ ^ ^
//                       | | +-- ds=0: status reports not routed to TE
//                       | +---- bfr=0: flush buffer on mode change
//                       +------ (these are the 4th and 5th params)
```

The 4th parameter (`ds`) controls status report routing:

- `0` = buffer in modem, no indication to TE (current)
- `1` = route as `+CDS: <length><CR><LF><pdu>` URC (full PDU inline)
- `2` = store in memory and route as `+CDSI: "SM",<index>` (like `+CMTI`)

## Plan

### 1. Enable TP-SRR in SMS-SUBMIT PDU

In `buildSmsSubmitPdu` (`sms_codec.cpp`), set bit 5 of the first octet:

```
GSM-7 path:   0x01 -> 0x21  (SMS-SUBMIT + SRR)
UCS-2 path:   0x01 -> 0x21  (same; neither path sets UDHI today)
```

If/when concat TX is implemented (UDHI bit 6 = 0x40), the combined
value would be `0x61` (SMS-SUBMIT + UDHI + SRR).

Add a `bool requestStatusReport` parameter to `buildSmsSubmitPdu`
(defaulting to `true`) so tests can exercise both paths and a future
"don't request reports" preference is trivial.

### 2. Capture TP-MR from `AT+CMGS` response

The modem responds to a successful `AT+CMGS` with:

```
+CMGS: <mr>\r\n
\r\n
OK\r\n
```

where `<mr>` is the Message Reference (0-255) assigned by the modem.

**Change `IModem::sendPduSms` signature** to return the MR on success:

```cpp
// Returns -1 on failure, 0-255 on success (the TP-MR assigned by the modem).
virtual int sendPduSms(const String &pduHex, int tpduLen) = 0;
```

This is a breaking interface change. `SmsSender::send` and all call
sites (`FakeModem`, `RealModem`) need updating. The old bool-returning
signature can be removed in the same changeset since it has exactly two
real callers.

**In `RealModem::sendPduSms`**, capture the response text from
`waitResponse` and parse `+CMGS: <mr>`:

```cpp
String resp;
int8_t rc = modem_.waitResponse(60000UL, resp);
if (rc != 1) return -1;
int cmgsIdx = resp.indexOf("+CMGS:");
if (cmgsIdx < 0) return -1;
int mr = resp.substring(cmgsIdx + 6).trim().toInt();
return mr;  // 0-255
```

### 3. Correlate outbound MR to sender phone + Telegram context

Add a new `DeliveryReportMap` class (similar in spirit to
`ReplyTargetMap`):

```cpp
class DeliveryReportMap {
public:
    static constexpr size_t kSlotCount = 32;
    // MR is 8-bit (0-255), so 32 slots covers the realistic
    // in-flight window with room to spare.

    struct Pending {
        uint8_t mr;
        char phone[23];       // destination number
        int32_t telegramMsgId; // the confirmation message we posted
        uint32_t timestamp;    // millis() when sent, for TTL expiry
    };

    // Store a pending delivery report.
    void put(uint8_t mr, const String &phone, int32_t telegramMsgId,
             uint32_t now);

    // Look up by MR. Returns true if found and not expired.
    bool lookup(uint8_t mr, Pending &out) const;

    // Evict entries older than kTtlMs.
    void evictExpired(uint32_t now);

    static constexpr uint32_t kTtlMs = 3600000; // 1 hour
};
```

Design notes:

- **No NVS persistence.** Delivery reports are ephemeral; if the device
  reboots, any in-flight reports are lost and the user simply never sees
  "Delivered". This is acceptable because (a) the "sent" confirmation
  has already been posted, (b) reboots are rare, and (c) the alternative
  (persisting a 32-slot buffer on every SMS send) adds flash wear for
  marginal value.
- **32 slots** is generous for the realistic traffic pattern (a few
  replies per day). The ring buffer is indexed by `mr % kSlotCount` with
  a stored-MR match check, same pattern as `ReplyTargetMap`.
- **1-hour TTL.** If the SMSC hasn't reported back in an hour, silently
  expire the slot. Some carriers never report at all (see Risks).

### 4. Configure modem to route status reports

Change the `AT+CNMI` setup in `main.cpp` from:

```
+CNMI=2,1,0,0,0
```

to:

```
+CNMI=2,1,0,1,0
```

The 4th parameter `ds=1` tells the modem to route status reports as
`+CDS` URCs containing the full SMS-STATUS-REPORT PDU inline:

```
+CDS: <length>\r\n
<hex PDU>\r\n
```

**Why `ds=1` (inline PDU) and not `ds=2` (store + index)?** With `ds=2`
we would get `+CDSI: "SM",<index>` and need a second `AT+CMGR` round
trip to fetch the PDU -- adding latency and complexity for no benefit.
The status report PDU is short (typically 28-34 bytes hex) and fits
easily in a single URC line. `ds=1` gives us the PDU immediately. The
only downside is that if the ESP is busy processing something else when
the URC arrives, serial buffer overflow could lose it -- but our serial
drain loop runs every 50ms, and at 115200 baud even a 34-byte hex PDU
(68 characters) takes <1ms to transmit, so overflow is not a realistic
concern.

Also send `AT+CSMP=49,167,0,0` or use `AT+CSMS=1` if needed to enable
the modem's status report capability. The A76xx documentation indicates
`AT+CSMS=1` (Phase 2+ mode) is required for the modem to process
incoming status reports. Add this to the setup sequence:

```cpp
modem.sendAT("+CSMS=1");
modem.waitResponse(2000);
```

Verify empirically on hardware that the A76xx actually delivers `+CDS`
URCs after these settings. If not, fall back to `ds=2` with `+CDSI` and
fetch via `AT+CMGR`.

### 5. Parse SMS-STATUS-REPORT PDU

Add `sms_codec::parseStatusReportPdu` to `sms_codec.{h,cpp}`:

```cpp
struct StatusReport {
    uint8_t messageRef;   // TP-MR: correlates to the outbound SMS
    String recipient;     // TP-RA: destination address (for display)
    String scTimestamp;    // TP-SCTS: when SMSC received the SMS
    String dischargeTime; // TP-DT: when the disposition was determined
    uint8_t status;       // TP-ST: 0x00 = delivered, others = failure
    bool delivered;       // convenience: status == 0x00
    String statusText;    // human-readable status description
};

bool parseStatusReportPdu(const String &hexPdu, StatusReport &out);
```

SMS-STATUS-REPORT PDU layout (3GPP TS 23.040 section 9.2.2.3):

```
[SCA length] [SCA]
[First octet]   -- TP-MTI=10 (status report)
[TP-MR]         -- Message Reference (1 byte, matches the outbound MR)
[TP-RA]         -- Recipient Address (BCD, same encoding as TP-OA)
[TP-SCTS]       -- Service Centre Time Stamp (7 bytes, BCD semi-octets)
[TP-DT]         -- Discharge Time (7 bytes, BCD semi-octets)
[TP-ST]         -- Status (1 byte)
```

TP-ST values (3GPP TS 23.040 section 9.2.3.15):

| Range     | Meaning                                    |
|-----------|--------------------------------------------|
| 0x00      | Short message received (delivered)         |
| 0x01      | Forwarded but delivery unconfirmed         |
| 0x02      | Replaced by SC                             |
| 0x20-0x2F | Temporary error, SC still trying           |
| 0x40-0x4F | Permanent error, SC stopped trying         |
| 0x60-0x6F | Temporary error, SC stopped trying         |

Map these to human-readable strings:

- `0x00` -> "delivered"
- `0x01` -> "forwarded, unconfirmed"
- `0x02` -> "replaced"
- `0x20` -> "temporary failure, still trying (congestion)"
- `0x21` -> "temporary failure, still trying (SME busy)"
- `0x22` -> "temporary failure, still trying (no response from SME)"
- `0x23` -> "temporary failure, still trying (service rejected)"
- `0x40` -> "permanent failure (remote procedure error)"
- `0x41` -> "permanent failure (incompatible destination)"
- `0x42` -> "permanent failure (connection rejected by SME)"
- `0x43` -> "permanent failure (not obtainable)"
- `0x44` -> "permanent failure (quality unavailable)"
- `0x45` -> "permanent failure (no interworking available)"
- `0x60` -> "temporary failure, stopped trying (congestion)"
- `0x61` -> "temporary failure, stopped trying (SME busy)"
- Other   -> "unknown status 0xNN"

### 6. Handle `+CDS` URC in the main loop

Add a new dispatch branch in `loop()`, **before** the
`callHandler.onUrcLine(line)` call (since `+CDS:` lines start with `+`,
not `RING`, the call handler would ignore them anyway, but placing
status report handling before call handling maintains logical grouping
with SMS):

```cpp
if (line.startsWith("+CDS:"))
{
    // +CDS: <length>\r\n
    // <hex PDU>\r\n
    // Read the next line from SerialAT to get the PDU.
    String pduLine;
    unsigned long deadline = millis() + 2000;
    while (millis() < deadline)
    {
        if (SerialAT.available())
        {
            pduLine = SerialAT.readStringUntil('\n');
            pduLine.trim();
            if (pduLine.length() > 0)
                break;
        }
        delay(1);
    }
    if (pduLine.length() > 0)
    {
        deliveryReportHandler.handleStatusReportPdu(pduLine);
    }
}
```

Note: `+CDS` is a two-line URC. The first line carries the length, the
second carries the hex PDU. This requires special handling compared to
single-line URCs like `+CMTI` and `+CLIP`. The existing
`readStringUntil('\n')` drain will consume the first `+CDS:` line; we
need to immediately read the next line for the PDU payload.

### 7. Post delivery notification to Telegram

Create a `DeliveryReportHandler` class (follows the `SmsHandler` /
`CallHandler` pattern):

```cpp
class DeliveryReportHandler {
public:
    DeliveryReportHandler(IBotClient &bot, DeliveryReportMap &map,
                          ClockFn clock);

    // Called when a status report PDU is received.
    void handleStatusReportPdu(const String &hexPdu);

private:
    IBotClient &bot_;
    DeliveryReportMap &map_;
    ClockFn clock_;
};
```

`handleStatusReportPdu` logic:

1. Parse the hex PDU via `sms_codec::parseStatusReportPdu`.
2. Look up the MR in the `DeliveryReportMap`. If not found (expired or
   never stored -- e.g., the report is for an SMS sent before the bridge
   was flashed with this firmware), log and discard silently.
3. If found, post a Telegram notification:
   - Delivered: "Delivered to +86 138-0000-1234"
   - Temporary failure, still trying: "Delivery in progress to
     +86 138-0000-1234 (SMSC still trying: <reason>)"
   - Permanent failure: "Delivery failed to +86 138-0000-1234:
     <reason>"
4. Remove the slot from the map (one report per outbound SMS; the
   SMSC may send multiple interim reports, but we only act on the
   first one for simplicity).

### 8. Wire it together in `main.cpp`

1. Construct `DeliveryReportMap` and `DeliveryReportHandler` as statics
   alongside the other handler singletons.
2. Update `SmsSender` to accept a `DeliveryReportMap*` (nullable; no
   map = no delivery tracking). After a successful `sendPduSms`, store
   the MR in the map along with the destination phone and `millis()`.
3. Periodically call `deliveryReportMap.evictExpired(millis())` from
   `loop()` -- can piggyback on the existing WiFi-check timer (every
   30s).
4. The `telegramMsgId` field in the `Pending` struct is the Telegram
   message_id of the "Reply sent to ..." confirmation. To populate
   this, `TelegramPoller::processUpdate` needs to switch from
   `bot_.sendMessage(...)` to `bot_.sendMessageReturningId(...)` for
   the confirmation and pass the returned id to the map. This lets a
   future enhancement **edit** the confirmation message in-place
   ("Reply sent" -> "Delivered") instead of posting a separate
   notification, but that's a stretch goal -- the first cut posts a
   new message.

### 9. Modify `buildSmsSubmitPdu` signature

Add an optional parameter to request status reports:

```cpp
bool buildSmsSubmitPdu(const String &phone, const String &body,
                       SmsSubmitPdu &out,
                       bool requestStatusReport = true);
```

When `requestStatusReport` is true, OR bit 5 into the first octet
(`0x01 | 0x20 = 0x21`). When false, leave it as `0x01`. Default to
true so all existing callers get delivery reports without code changes.

### 10. Tests

All new code should be host-testable in `test/test_native/`:

- **`parseStatusReportPdu`**: exercise with canned hex PDUs covering
  delivered (0x00), temporary failure (0x20), permanent failure (0x40),
  truncated PDU, and non-status-report MTI.
- **`DeliveryReportMap`**: put/lookup/evict/collision semantics,
  modeled on the existing `ReplyTargetMap` tests.
- **`DeliveryReportHandler`**: wire a `FakeBotClient` and assert the
  correct Telegram message is posted for delivered/failed/unknown-MR
  scenarios.
- **`buildSmsSubmitPdu` with SRR**: verify the first octet is `0x21`
  when `requestStatusReport=true` and `0x01` when false.
- **`RealModem::sendPduSms` MR extraction**: cannot be tested on host
  (depends on TinyGSM), but document the expected `+CMGS: <mr>` format
  and test the string-parsing helper in isolation.

## Risks

### Not all carriers relay status reports

Many carriers, particularly in North America, simply do not deliver
status reports for inter-carrier messages. The user will see "Reply
sent" but never get a "Delivered" follow-up. This is **not a bug** --
it's the expected behavior when the SMSC doesn't cooperate. We should
document this in the bot's help text and NOT retry or alarm on the
absence of a report.

The 1-hour TTL in `DeliveryReportMap` silently handles this: the slot
expires and no notification is posted.

### Some carriers always report "delivered" even when they didn't

Some SMSCs report `TP-ST=0x00` (delivered) the moment they accept the
message from the originating network, before the message actually
reaches the destination handset. This gives the user a false sense of
delivery confirmation. There is nothing we can do about this at the
firmware level -- the SMSC is lying. The Telegram notification should
use cautious language: "Delivered to" rather than "Received by" to
reflect that this is the SMSC's report, not the recipient's ack.

### TP-MR is only 8 bits

Message Reference wraps around at 255. If the user sends 256+ SMS
replies within the 1-hour TTL window, the ring buffer slot for an old
MR could be overwritten by a new one, and a late-arriving status report
for the old SMS would be matched to the wrong outbound message. At
realistic traffic volumes for a personal bridge (a few replies per day),
this is vanishingly unlikely. The 32-slot ring buffer with a stored-MR
match check provides additional protection: only exact MR matches are
accepted.

If this ever becomes a real concern, the mitigation is to also match on
the TP-RA (recipient address) from the status report against the stored
phone number. This is a strict superset of the MR-only match and
eliminates false positives entirely.

### Two-line URC parsing

`+CDS` is a multi-line URC (length on first line, PDU on second). The
current `readStringUntil('\n')` loop in `main.cpp` processes one line
at a time. We need to handle the case where the second line hasn't
arrived yet when we process the first. The approach in Plan section 6
(blocking read with a 2-second deadline) is simple and robust, but it
does block the main loop briefly. Since status reports are infrequent
(at most one per outbound SMS, typically seconds to minutes after
sending), this is acceptable.

### Interaction with `+CMTI` for status reports stored on SIM

If we use `ds=2` instead of `ds=1` (because `ds=1` doesn't work on the
A76xx in practice), the status report is stored on the SIM and indicated
via `+CDSI: "SM",<index>`. This index occupies the same SIM storage
space as normal SMS. The existing `sweepExistingSms` at boot would try
to parse these as SMS-DELIVER PDUs and fail. If we go the `ds=2` route,
`handleSmsIndex` needs to detect SMS-STATUS-REPORT PDUs (TP-MTI = 10)
and route them to the delivery report handler instead of the SMS
pipeline.

## Notes for handover

1. **Start with `AT+CSMS=1` and `+CNMI=2,1,0,1,0` on real hardware.**
   Verify the modem actually emits `+CDS` URCs after sending an SMS
   with TP-SRR set. If it doesn't, try `ds=2` with `+CDSI`. The A76xx
   AT command manual (SIMCom document) is the authority; the behavior
   may vary between A7670G and A7670E firmware versions.

2. **The `sendPduSms` return-type change is the riskiest part.** It
   touches `IModem` (the narrow interface), `RealModem`, `FakeModem`,
   `SmsSender`, and all tests. Do this in a standalone commit with a
   `FakeModem` that returns a predictable MR sequence (0, 1, 2, ...)
   so existing tests don't break.

3. **The `+CDS` two-line URC is the trickiest parsing challenge.** All
   other URCs in the system (`+CMTI`, `+CLIP`, `RING`) are single-line.
   Consider whether the main-loop drain should be refactored into a
   small state machine that can buffer a partial multi-line URC, rather
   than the blocking-read approach in the plan. This is a judgment call
   -- the blocking approach is simpler and status reports are rare.

4. **Stretch goal: edit the confirmation message in place.** Instead of
   posting a separate "Delivered" notification, use Telegram's
   `editMessageText` API to update the original "Reply sent to ..."
   message to "Reply sent to ... -- delivered". This requires storing
   the confirmation message_id in the `DeliveryReportMap` (already in
   the plan) and adding an `editMessage` method to `IBotClient`. Not
   essential for the first cut.

5. **Stretch goal: interim reports.** Some SMSCs send multiple status
   reports for the same MR (e.g., "still trying" followed by
   "delivered"). The first-cut plan acts on only the first report and
   evicts the slot. A more complete implementation would keep the slot
   alive until a final status (delivered or permanent failure) and
   update the Telegram notification with each interim status. Low
   priority given that most carriers skip interim reports entirely.

## Review

verdict: approved-with-changes

### Protocol correctness

- (NON-BLOCKING) **TP-SRR bit calculation is correct.** `0x01` is
  `0b00000001`; bit 5 (zero-indexed from LSB) set gives `0b00100001 =
  0x21`. The table in the RFC is accurate. The combined UDHI+SRR value
  of `0x61` for concat TX is also correct (`0x01 | 0x20 | 0x40`).

- (NON-BLOCKING) **TP-ST range table has a gap and a minor inaccuracy.**
  3GPP TS 23.040 §9.2.3.15 defines `0x60-0x6F` as "Temporary error, SC
  not making any more transfer attempts" (i.e., the SC has given up, same
  as the 0x40 permanent range but recoverable in principle). The RFC
  describes these as "Temporary error, stopped trying" -- accurate
  enough, but the human-readable strings list only `0x60` and `0x61`
  while the table claims coverage through `0x6F`. The parser should have
  a range-based fallback for `0x62`-`0x6F` (and similarly `0x22`-`0x2F`,
  `0x42`-`0x4F`), not just the spot-mapped entries. Without it, any
  sub-code other than the handful listed yields "unknown status 0xNN",
  which is misleading when the range meaning is known.

### `+CDS` two-line URC parsing

- (BLOCKING) **The blocking-read approach in Plan §6 has a race
  condition.** The main loop's `readStringUntil('\n')` runs inside
  `while (SerialAT.available())`. When `+CDS: <length>` arrives and is
  consumed as `line`, the PDU line may not yet be in the UART RX buffer
  -- at 115200 baud, 68 hex chars take ~6 ms to arrive after the first
  line's `\n`. The proposed inner loop spins on `SerialAT.available()`
  with `delay(1)` up to a 2-second deadline, which will normally work,
  but it does so inside the outer `while (SerialAT.available())` loop
  which exits when available() is false, so control never even reaches
  the inner loop if the PDU bytes haven't arrived yet. The fix: the
  `+CDS` branch must be placed **outside** (or tested to break out of)
  the `while (SerialAT.available())` loop, or -- simpler -- moved to its
  own polling path that always tries to drain a pending PDU line on the
  next iteration. As written, the pseudo-code in the RFC shows the inner
  read running while still inside the outer while, which is correct
  structurally, but the comment "Read the next line from SerialAT" must
  be explicit that this inner loop runs regardless of `available()`. The
  RFC's handover note 3 calls this out as the trickiest part, but the
  plan itself does not provide a safe concrete pattern -- this needs
  explicit resolution before implementation, not a shrug to the
  implementer.

- (NON-BLOCKING) **The `+CDS` branch should also be guarded against
  feeding the consumed `pduLine` to `callHandler.onUrcLine`.** Currently
  the RFC adds the branch before the `callHandler` feed, which is
  correct. But there is no explicit `continue` after handling `+CDS`, so
  the `+CDS:` header line would still fall through to
  `callHandler.onUrcLine(line)`. `CallHandler` filters on `RING` and
  `+CLIP:` so it would silently ignore it, but the intent should be made
  explicit with a `continue` or an `else if` chain.

### `AT+CNMI=2,1,0,1,0` parameter change

- (BLOCKING) **The `ds=1` change is backwards-compatible with `mt=1`,
  but the RFC does not verify `bfr` semantics.** All five CNMI
  parameters are sent atomically. The existing command is
  `+CNMI=2,1,0,0,0` (mode=2, mt=1, bm=0, ds=0, bfr=0). Changing to
  `+CNMI=2,1,0,1,0` only alters `ds`; the remaining parameters are
  identical, so `+CMTI` delivery is unaffected. This part is fine.
  However, the RFC does not address whether `AT+CSMS=1` (Phase 2+ mode,
  Plan §4) might alter the interpretation of `+CNMI` parameters or
  cause the modem to re-emit buffered status reports stored before
  `AT+CSMS=1` was issued. This needs empirical verification on the
  A76xx; the RFC acknowledges it but should elevate it to a hard
  pre-condition check rather than a footnote.

- (NON-BLOCKING) **`AT+CSMP` is mentioned but never fully specified.**
  Plan §4 says "Also send `AT+CSMP=49,167,0,0` or use `AT+CSMS=1`" with
  "if needed" hedging. `AT+CSMP` is a text-mode command and irrelevant
  when `+CMGF=0` (PDU mode). It should be removed from the plan entirely
  to avoid confusion. `AT+CSMS=1` is the relevant command.

### `sendPduSms` return-type change

- (BLOCKING) **The interface change is straightforward but the RFC
  understates the test breakage.** Callers are: `IModem` (interface
  declaration), `RealModem::sendPduSms` (returns `bool`), `FakeModem::
  sendPduSms` (returns `bool` from a `std::vector<bool>` queue), and
  `SmsSender::send` (stores result in `bool ok`). The test file
  `test_sms_sender.cpp` at line 33 asserts the PDU first octet is `"01"`
  (`hex.startsWith("000100")`). If SRR is enabled by default
  (`requestStatusReport = true` as the RFC proposes), that assertion
  becomes `"000121"` -- the test **will fail** after the change. This
  needs explicit acknowledgement: adding SRR by default is a breaking
  change to the existing test baseline, not just a mechanical type
  change. The RFC should flag this test as requiring an update.

- (NON-BLOCKING) **The `FakeModem` queue change is non-trivial.** It
  currently queues `bool` results and returns an MR stub of 0 on
  success. Tests that assert `pduSendCalls().size() == 1` will still
  pass, but any test that needs to exercise the MR-capture path will need
  to queue integer results. The `FakeModem` struct `PduSendCall` may also
  want to record the first-octet value so tests can assert TP-SRR is set
  without re-decoding the full hex PDU.

- (NON-BLOCKING) **The MR-parsing snippet in Plan §2 uses
  `resp.substring(cmgsIdx + 6).trim().toInt()`.** `String::toInt()` on
  Arduino returns 0 for non-numeric input and for the value 0 itself --
  both `MR=0` (a valid first message) and a parse failure produce 0.
  The RFC should use a sentinel like -2 for parse failure, or detect a
  missing `+CMGS:` line separately from a zero MR. As written, if
  `waitResponse` returns 1 (OK) but the `+CMGS:` line is absent (some
  modem firmware variants omit it), the function returns 0, which
  `SmsSender` stores as a valid MR and puts in the ring buffer -- a
  silent false match. A safer pattern: return -1 if `cmgsIdx < 0`, else
  return `resp.substring(cmgsIdx + 6).trim().toInt()` and document that
  MR=0 is valid.

### MR collision and the ring buffer

- (NON-BLOCKING) **32 slots indexed by `mr % 32` with a stored-MR match
  check is sound for the stated traffic.** The additional mitigation of
  also matching on TP-RA (recipient address) is called out as future
  work and should be the recommended path if the feature is enabled on a
  higher-traffic device.

- (NON-BLOCKING) **The 1-hour TTL in `DeliveryReportMap` silently
  handles non-cooperating carriers, but the user is left in the dark.**
  After the "Reply sent" confirmation, the user never knows whether
  silence means the carrier swallowed the report or the message was
  never delivered. Consider adding a note to the Telegram "Reply sent"
  message that delivery confirmation depends on carrier support -- this
  sets expectations before the TTL silently expires.

### Carrier reality and compile-time opt-in

- (NON-BLOCKING) **The feature should have a compile-time opt-in flag.**
  The RFC acknowledges that many carriers, especially in North America,
  do not relay status reports. Enabling TP-SRR unconditionally means
  every outbound SMS carries the SRR bit even when the carrier ignores
  it -- harmless, but it changes the PDU format globally. More
  importantly, the `DeliveryReportMap`, `DeliveryReportHandler`, and the
  `+CDS` URC branch all add RAM and flash overhead permanently. A
  `#ifdef ENABLE_DELIVERY_REPORTS` guard (or a `platformio.ini`
  `-D` build flag, consistent with the existing `-DALLOW_INSECURE_TLS`
  pattern) would let users on non-cooperating carriers skip the feature
  entirely. The `requestStatusReport` parameter on `buildSmsSubmitPdu`
  is a step in this direction but stops short of gating the full
  infrastructure.

### Complexity vs. value

- (NON-BLOCKING) **A simpler first cut is viable and should be
  considered.** The minimal useful version is: parse `+CDS` URCs, log
  the MR and TP-ST to Serial, post a Telegram message if the MR matches
  something in the ring buffer. This is roughly steps 1-4 and 6-7,
  skipping the `telegramMsgId` in-place edit stretch goal entirely. The
  `DeliveryReportMap` can start without NVS persistence. This cuts scope
  by ~30% while delivering the core user value (knowing if delivery
  failed). The stretch goals (edit-in-place, interim reports) can be a
  follow-on RFC. The RFC's own "Notes for handover" implicitly endorses
  this ordering -- the reviewer agrees.

### Interaction with RFC-0009 (concat TX)

- (BLOCKING) **The RFC does not adequately address the concat TX
  interaction.** RFC-0009 is listed as a soft dependency in the RFC list
  (it appears after 0011 and is `proposed`), but the interaction goes in
  both directions: if RFC-0009 is implemented first or concurrently,
  each part of a multipart SMS gets its own MR and its own `+CDS` status
  report. The `DeliveryReportMap` as designed stores one entry per
  outbound `SmsSender::send` call, but with concat TX, one `send` call
  produces N PDUs with N distinct MRs. The ring buffer would need N
  entries for a single user-visible message, and the "delivered"
  notification should fire only when all N parts are confirmed -- or at
  minimum, fire per-part with a part number in the notification. The RFC
  waves at this with "If/when concat TX is implemented, the combined
  value would be 0x61" but says nothing about the MR multiplicity
  problem. This must be resolved before concurrent implementation of
  both RFCs, otherwise the delivery-report infrastructure will be
  silently wrong for multipart messages. Recommend: if RFC-0009 ships
  first, RFC-0011 must be scoped to single-part-only with an explicit
  guard; if RFC-0011 ships first, RFC-0009 must be updated to note the
  delivery-report interaction.

### Test plan

- (NON-BLOCKING) **The test plan is realistic for the host-testable
  portions.** `parseStatusReportPdu`, `DeliveryReportMap`, and
  `DeliveryReportHandler` are all pure logic that can be exercised with
  canned hex PDUs and `FakeBotClient`, consistent with the existing test
  architecture. The note that `RealModem::sendPduSms` MR extraction
  "cannot be tested on host" is correct and the suggested workaround
  (test the string-parsing helper in isolation) is the right approach.
  One gap: there is no test plan entry for the `+CDS` URC dispatch in
  `main.cpp`. That code is in the hardware-dependent TU and cannot be
  unit-tested, but it is the trickiest part of the implementation (see
  the blocking issue above on the two-line URC race). An integration test
  note or a hardware verification checklist would strengthen the plan.

### Summary

The RFC is well-motivated and the core protocol design is sound -- the
TP-SRR bit value is correct, the PDU layout for SMS-STATUS-REPORT is
accurate, and the `DeliveryReportMap` ring-buffer approach is a sensible
match for the `ReplyTargetMap` pattern already in the codebase. However,
three blocking issues must be resolved before implementation begins:
(1) the `+CDS` two-line URC parsing plan has a latent race condition
that needs a concrete, safe reading pattern rather than a note-to-self;
(2) the return-type change to `sendPduSms` will break the existing
`test_sms_sender.cpp` PDU-format assertion and this must be called out
explicitly; and (3) the interaction with RFC-0009 concat TX is
unaddressed -- the two RFCs must agree on which ships first and what
the other one must guard against. Resolving these three issues and
adding a compile-time opt-in flag will make this approvable for
implementation.

## Code Review

verdict: approved-with-changes

### 1. `+CMGS: <mr>` parsing in `real_modem.h` (lines 64-71)

**NON-BLOCKING.** The parser (`resp.substring(cmgsIdx + 6).trim().toInt()`) correctly handles both `+CMGS: 42` (space after colon) and `+CMGS:42` (no space), because `.trim()` strips leading whitespace before `.toInt()`. The `+CMGS:` literal is 6 characters and `cmgsIdx + 6` correctly lands on the character immediately after the colon. No whitespace bug here.

**BLOCKING.** The fallback `return 0` on line 71 (OK returned but no `+CMGS:` line found) is a silent false match. MR=0 is a valid modem-assigned reference, so `SmsSender::send` will call `deliveryReportMap_->put(0, phone, ...)` and a future delivery report for a legitimate MR=0 could match this phantom entry. The fix: return `-1` when `cmgsIdx < 0` (absent `+CMGS:` line), same as the failure path. The comment currently says "return 0 as a safe default" but 0 is not safe — it is a valid MR value.

### 2. `parseStatusReportPdu` PDU byte layout in `sms_codec.cpp` (lines 1306-1391)

**NON-BLOCKING (correct).** The offset arithmetic through the variable-length RA field is sound: digit count is read as `raDigits`, TON/NPI byte read next, then `raBytes = (raDigits + 1) / 2` is computed and bounds-checked (`pos + raBytes > raw.size()`), BCD decoded, and `pos += raBytes` advances past the field. The SCTS and DT 7-byte fields are each bounds-checked before consuming. No off-by-one detected.

**NON-BLOCKING.** The `statusText` helper covers `0x20-0x2F`, `0x40-0x4F`, and `0x60-0x6F` by range but only names a handful of sub-codes within each range; unlisted sub-codes fall through to a generic "temporary/permanent failure (0xNN)" string, which is correct and does not cause incorrect behavior. The existing `## Review` section flags this as a non-blocking improvement opportunity; confirmed.

### 3. `DeliveryReportMap` ring buffer (`delivery_report_map.h`)

**NON-BLOCKING (correct).** The ring buffer stores the full 8-bit MR in each slot and `lookup()` checks `slots_[idx].mr != mr` before returning the phone number. A wrap-around collision (e.g., MR=0 and MR=32 both map to slot 0) causes the later `put()` to overwrite the slot; a subsequent `lookup(0, ...)` correctly returns false because the stored MR is now 32. The original slot is not consumed on a miss. The `test_DeliveryReportMap_lookup_wrong_mr_returns_false` test and `test_DeliveryReportMap_collision_overwrites_old_slot` test both cover this path explicitly.

### 4. `waitingCdsPdu` state machine placement in `main.cpp` (lines 570-594)

**NON-BLOCKING (correct).** `static bool waitingCdsPdu = false` is declared at line 571 outside the `while (SerialAT.available())` loop but inside `loop()`. The static storage duration means the flag persists across `loop()` calls. When `+CDS: <length>` is seen, `waitingCdsPdu = true` and the loop `continue`s. If `SerialAT.available()` then returns 0 (PDU bytes not yet in the buffer), the while-loop exits with `waitingCdsPdu` still true. On the next call to `loop()`, the `if (waitingCdsPdu)` check runs on the first non-empty line received, correctly picking up the PDU hex. This resolves the race-condition concern raised in the existing `## Review` section — the implementation does not use the blocking inner-loop approach described in Plan §6; instead it uses the persistent-flag approach, which is correct.

The `+CDS:` header line also correctly `continue`s without falling through to `callHandler.onUrcLine(line)`, addressing the non-blocking fall-through concern from the existing Review.

### 5. `DeliveryReportHandler::onStatusReport` (`delivery_report_handler.cpp`)

**NON-BLOCKING (correct).** Parse failure returns early with a Serial log and no Telegram message. Map miss (expired or unknown MR) also returns early with a Serial log and no Telegram message. The temporary-failure branch (status `0x20`-`0x2F`) consumes the map slot on the first interim report — subsequent interim reports for the same MR will get a "not found in map" log and be silently dropped. This matches the stated design ("act on only the first report for simplicity"). Note that for temporary failures the slot is still consumed even though delivery has not yet been confirmed; if a final "delivered" report later arrives for the same MR, it will be silently dropped. This is a known limitation documented in RFC Notes §5 ("stretch goal: interim reports").

### 6. `SmsSender::setDeliveryReportMap` wiring in `main.cpp` and `sms_sender.h`

**NON-BLOCKING (correct).** `deliveryReportMap` is a file-scope static at line 78 of `main.cpp`, alive for the process lifetime. `smsSender.setDeliveryReportMap(&deliveryReportMap)` is called at line 327 inside the `#ifdef ENABLE_DELIVERY_REPORTS` block in `setup()`. `setDeliveryReportMap()` is defined inline in `sms_sender.h` (lines 89-92). All wiring is correct.

### 7. SRR bit (`requestStatusReport`) not passed through in `sms_sender.cpp`

**BLOCKING.** `SmsSender::send` calls `sms_codec::buildSmsSubmitPduMulti(number, body)` at line 35 of `sms_sender.cpp` with no `requestStatusReport` argument. The default value of that parameter is `false`, so TP-SRR is **never set in any outbound PDU**, even when `-DENABLE_DELIVERY_REPORTS` is defined. The modem will not request a delivery report from the SMSC, so no `+CDS` URC will ever be generated. The entire feature is silently inert despite the infrastructure being in place. Fix: change the call to `buildSmsSubmitPduMulti(number, body, 10, deliveryReportMap_ != nullptr)` or add a `#ifdef ENABLE_DELIVERY_REPORTS` block that passes `true`. Either approach should also add or update a test in `test_sms_sender.cpp` asserting the first octet is `0x21` when SRR is enabled (currently line 37 asserts `"000100"` which confirms SRR is currently off).

As a side-effect, this also means the `test_sms_sender.cpp` line 37 assertion (`hex.startsWith("000100")`) does not break, which is why the pre-existing Review concern about test breakage has not materialized — but that is because the feature is broken, not because the tests are right.

### 8. `+CDS` state machine test coverage gap

**NON-BLOCKING.** The 22 new tests in `test_delivery_report.cpp` cover `DeliveryReportMap`, `parseStatusReportPdu`, `DeliveryReportHandler`, and the SRR bit in `buildSmsSubmitPduMulti` exhaustively. However, there is no test that exercises the two-line `+CDS` URC dispatch in `loop()` — the code path that sets and reads `waitingCdsPdu`. That code is in the hardware-dependent `main.cpp` TU and cannot be unit-tested against the native env. The `## Review` section flagged this gap; the implementation has not added a hardware verification checklist or integration test note for it.

### Summary

The implementation is largely well-executed: the PDU parser advances offsets correctly, the ring buffer aliasing is handled properly, the `waitingCdsPdu` state machine correctly survives the inter-loop-call timing gap (resolving the blocking concern from the prior Review), the handler error paths are clean, and the `#ifdef ENABLE_DELIVERY_REPORTS` guards are applied consistently. Two issues require resolution before the feature can be considered functional: (1) the `return 0` fallback in `RealModem::sendPduSms` when no `+CMGS:` line is found conflates a parse failure with a valid MR=0, silently inserting a phantom ring-buffer entry — change it to `return -1`; (2) `SmsSender::send` never passes `requestStatusReport=true` to `buildSmsSubmitPduMulti`, meaning TP-SRR is never set and the modem will never generate `+CDS` URCs — the entire delivery-report path is dead code until this one-liner is fixed. Fix both and the feature is ready for hardware validation.
