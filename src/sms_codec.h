#pragma once

// Pure (Arduino-String-only) helpers for the SMS pipeline.
//
// This translation unit intentionally has NO hardware dependencies
// beyond `String` — it must compile unchanged against the host-side
// stub in test/support/Arduino.h so the native test env can exercise
// it directly.

#include <Arduino.h>
#include <stdint.h>

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

// Convert a CMGR timestamp ("yy/MM/dd,HH:mm:ss+zz") to RFC 3339.
// Returns the empty string if the input is shorter than 17 chars
// (we don't try to guess). The timezone is currently hardcoded to
// "+08:00" — matching what the previous implementation returned.
String timestampToRFC3339(const String &timestamp);

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

} // namespace sms_codec
