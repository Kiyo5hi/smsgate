#pragma once

// Pure (Arduino-String-only) helpers for the SMS pipeline.
//
// This translation unit intentionally has NO hardware dependencies
// beyond `String` — it must compile unchanged against the host-side
// stub in test/support/Arduino.h so the native test env can exercise
// it directly.

#include <Arduino.h>
#include <stdint.h>
#include <vector>

namespace sms_codec {

// Decode a hex string produced by the modem under CSCS="UCS2".
// Accepts whitespace / CR / LF between characters. If the input is not
// a well-formed even-length hex string, returns it unchanged (this is
// the GSM-7bit ASCII passthrough path). Surrogate pairs are decoded
// into 4-byte UTF-8 sequences. Length%4==0 is treated as UTF-16BE;
// length%2==0 but length%4!=0 is treated as raw ASCII bytes in hex.
String decodeUCS2(String hex);

// Parse an AT+CMGR text-mode response. Expected shape:
//   +CMGR: "REC UNREAD","<sender-hex>","","<timestamp>"\r\n
//   <content-hex>\r\n
//   \r\n
//   OK\r\n
//
// On success, fills sender (UTF-8), timestamp (raw, still
// "yy/MM/dd,HH:mm:ss+zz"), and content (UTF-8). Returns false if the
// header line is missing or the 8th quote can't be found.
bool parseCmgrBody(const String &raw, String &sender, String &timestamp, String &content);

// Format a raw phone number for display in the Telegram message.
//   11 digits              -> "+86 xxx-xxxx-xxxx"
//   13 chars starting +86  -> "+86 xxx-xxxx-xxxx"
//   anything else          -> unchanged
String humanReadablePhoneNumber(const String &number);

// RFC-0078: Strip formatting characters from a phone number entered by the user
// before passing it to the modem. Removes spaces, dashes, parentheses, and dots,
// keeping only '+' and digits. Also converts a leading "00" prefix to "+" (ITU
// alternative for international prefix). Examples:
//   "+44 7911-123 456"  -> "+447911123456"
//   "0044 7911 123456"  -> "+447911123456"
//   "(+1) 800-555-0100" -> "+18005550100"
//   "07911123456"       -> "07911123456" (local format, unchanged)
String normalizePhoneNumber(const String &raw);

// Convert a CMGR timestamp ("yy/MM/dd,HH:mm:ss+zz") to RFC 3339.
// Returns the empty string if the input is shorter than 17 chars
// (we don't try to guess). gmtOffsetHours defaults to +8 to preserve
// previous behaviour; pass the runtime value for RFC-0169 support.
// Range: -12 to +14 (all IANA standard UTC offsets).
String timestampToRFC3339(const String &timestamp, int gmtOffsetHours = 8);

// Parse a `+CLIP: "<number>",<type>,...` URC line, extracting the
// caller number field into `number`. Returns true if the line starts
// with `+CLIP:` and has a quoted number field (even an empty one);
// false on malformed input (no quotes, no colon, etc.).
//
// The parser is deliberately lenient about trailing fields — some
// A76xx firmware versions emit only `+CLIP: "<number>",<type>` with
// no trailing empty strings, others append extra fields. Everything
// past the first quoted string is ignored.
//
// For withheld / anonymous callers the modem emits `+CLIP: "",128,...`
// which yields `number = ""` and still returns true — the caller
// should treat an empty number as "unknown" and still emit a
// notification.
bool parseClipLine(const String &line, String &number);

// ---------- PDU mode (RFC-0002) ----------

// Parsed SMS-DELIVER PDU. All String fields are UTF-8.
struct SmsPdu
{
    // Sender phone number. For international format numbers, starts
    // with "+" (matches what text-mode CSCS="UCS2" would have given us
    // after UCS2 decoding of an international number).
    String sender;

    // Raw timestamp in the same "yy/MM/dd,HH:mm:ss+zz" shape that
    // timestampToRFC3339 already consumes. The "+zz" suffix is the
    // raw BCD timezone nibble as returned by the PDU (quarter hours);
    // timestampToRFC3339 currently ignores it and hardcodes +08:00.
    String timestamp;

    // Decoded message content (UTF-8).
    String content;

    // Concatenation metadata from the User Data Header, if present.
    bool isConcatenated = false;
    uint16_t concatRefNumber = 0; // 8-bit refs are widened to 16-bit
    uint8_t concatTotalParts = 0;
    uint8_t concatPartNumber = 0;
};

// Parse a hex-encoded SMS-DELIVER PDU as returned by AT+CMGR / AT+CMGL
// in PDU mode (AT+CMGF=0). Returns true on success, false on any
// malformed / truncated / unsupported PDU. Supports:
//   - TP-DCS 0x00 (GSM 7-bit default alphabet, packed)
//   - TP-DCS 0x04 (8-bit data, passed through as-is)
//   - TP-DCS 0x08 (UCS-2 / UTF-16BE)
//   - User Data Header concatenation IEIs 0x00 (8-bit ref) and 0x08 (16-bit ref)
//   - SCTS semi-octet timestamp
// Does NOT support: compressed messages, status reports, SMS-SUBMIT,
// USSD, flash / silent SMS special handling (they still decode; they
// just aren't flagged as such).
bool parseSmsPdu(const String &hexPdu, SmsPdu &out);

// ---------- PDU mode encoder (Unicode SMS TX) ----------

// Result of building an SMS-SUBMIT PDU.
struct SmsSubmitPdu
{
    String hex;    // Full PDU as hex string (SCA + TPDU)
    int tpduLen;   // TPDU byte count (everything after the SCA field);
                   // this is the value passed to AT+CMGS=<tpduLen>
};

// Pack an array of 7-bit septets into bytes (GSM-7 packing).
// bitOffset: number of bits to skip at the start of the output buffer
// before writing the first septet (used for concat UDH fill bits;
// default 0 for normal single-part PDUs).  The first bitOffset bits
// of the output are zero-filled, then septets are packed starting at
// that bit position.
//
// This is the inverse of unpackSeptets() on the RX side.
std::vector<uint8_t> packSeptets(const std::vector<uint8_t> &septets,
                                 int bitOffset = 0);

// Build an SMS-SUBMIT PDU for the given destination phone number and
// UTF-8 body.  Auto-selects GSM-7 (160 char capacity) when the body
// contains only characters in the GSM 7-bit default alphabet +
// extension table; falls back to UCS-2 / UTF-16BE (70 BMP-char /
// fewer with supplementary characters) otherwise.
//
// Single-PDU backward-compatible overload: returns false if the body
// won't fit in a single non-concatenated SMS (>160 GSM-7 septets or
// >140 UCS-2 octets) OR if the multi-part result has more than one
// part.  Kept for test compatibility; production callers should use
// buildSmsSubmitPduMulti.
bool buildSmsSubmitPdu(const String &phone, const String &body,
                       SmsSubmitPdu &out);

// Build a vector of SMS-SUBMIT PDUs for the given destination and
// UTF-8 body.  For bodies that fit in a single PDU (<=160 GSM-7
// septets or <=70 UCS-2 code units), returns a 1-element vector with
// NO UDH -- identical output to the single-PDU overload above.  For
// longer bodies, splits into multiple parts with concat UDH
// (IEI=0x00, 8-bit reference counter).
//
// When `requestStatusReport` is true, sets bit 5 (TP-SRR) in the
// first octet of each PDU (0x01->0x21 for single-part; 0x41->0x61
// for concat with UDHI). Used by SmsSender when -DENABLE_DELIVERY_REPORTS
// is defined. Default is false so the default build is unchanged.
//
// Returns an empty vector if:
//   - phone or body is empty
//   - the body requires more than maxParts parts (default 10)
//
// maxParts = 10 gives:
//   GSM-7:  10 x 153 = 1,530 characters
//   UCS-2:  10 x  67 =   670 characters
std::vector<SmsSubmitPdu> buildSmsSubmitPduMulti(const String &phone,
                                                  const String &body,
                                                  int maxParts = 10,
                                                  bool requestStatusReport = false);

// Return true iff every code point in the UTF-8 string `s` can be
// represented in the GSM 7-bit default alphabet (basic table +
// extension table).  Used by SmsSender to choose between GSM-7 (160
// char) and UCS-2 (70 char) and give a precise length error.
bool isGsm7Compatible(const String &s);

// Return the number of SMS parts `body` will require (RFC-0037).
// Returns 0 if the body is empty or exceeds maxParts (default 10).
// Uses buildSmsSubmitPduMulti internally so encoding selection is exact.
int countSmsParts(const String &body, int maxParts = 10);

// ---------- SMS-STATUS-REPORT PDU parser (RFC-0011) ----------

// Parsed SMS-STATUS-REPORT PDU (3GPP TS 23.040 section 9.2.2.3).
// All String fields are UTF-8.
struct StatusReport
{
    uint8_t messageRef;     // TP-MR: correlates to the outbound SMS MR
    String  recipient;      // TP-RA: destination address (for display)
    String  scTimestamp;    // TP-SCTS: when SMSC received the outbound SMS
    String  dischargeTime;  // TP-DT: when the disposition was determined
    uint8_t status;         // TP-ST: 0x00 = delivered, others = failure
    bool    delivered;      // convenience: status == 0x00
    String  statusText;     // human-readable status description
};

// Parse a hex-encoded SMS-STATUS-REPORT PDU as delivered in a +CDS URC.
// Returns true on success, false on any malformed / truncated PDU or
// if the TP-MTI field is not 0b10 (status report).
bool parseStatusReportPdu(const String &hexPdu, StatusReport &out);

} // namespace sms_codec
