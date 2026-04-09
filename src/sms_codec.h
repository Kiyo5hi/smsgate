#pragma once

// Pure (Arduino-String-only) helpers for the SMS pipeline.
//
// This translation unit intentionally has NO hardware dependencies
// beyond `String` — it must compile unchanged against the host-side
// stub in test/support/Arduino.h so the native test env can exercise
// it directly.

#include <Arduino.h>

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

} // namespace sms_codec
