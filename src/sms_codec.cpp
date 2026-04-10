#include "sms_codec.h"

#include <stdint.h>
#include <stddef.h>
#include <ctype.h>
#include <stdio.h>

#include <vector>

namespace sms_codec {

// ---------- formatting helpers ----------

String humanReadablePhoneNumber(const String &number)
{
    // xxxxxxxxxxx       -> +86 xxx-xxxx-xxxx
    // +86xxxxxxxxxxx    -> +86 xxx-xxxx-xxxx
    // anything else     -> unchanged

    if (number.length() == 11)
    {
        return "+86 " + number.substring(0, 3) + "-" + number.substring(3, 7) + "-" + number.substring(7);
    }
    else if (number.length() == 13 && number.startsWith("+86"))
    {
        return "+86 " + number.substring(3, 6) + "-" + number.substring(6, 10) + "-" + number.substring(10);
    }
    else
    {
        return number;
    }
}

String normalizePhoneNumber(const String &raw)
{
    String result;
    for (unsigned int i = 0; i < raw.length(); i++) {
        char c = raw[i];
        if (c == '+' || (c >= '0' && c <= '9'))
            result += c;
        // skip spaces, dashes, dots, parentheses, slashes
    }
    // Convert leading "00" international prefix to "+"
    if (result.length() >= 2 && result[0] == '0' && result[1] == '0')
        result = String("+") + result.substring(2);
    return result;
}

String timestampToRFC3339(const String &timestamp)
{
    // Input format:  "yy/MM/dd,HH:mm:ss+zz" (as returned in +CMGR headers)
    // Output format: "YYYY-MM-DDTHH:mm:ss+08:00"
    if (timestamp.length() < 17)
        return "";

    String year = "20" + timestamp.substring(0, 2);
    String month = timestamp.substring(3, 5);
    String day = timestamp.substring(6, 8);
    String hour = timestamp.substring(9, 11);
    String minute = timestamp.substring(12, 14);
    String second = timestamp.substring(15, 17);

    return year + "-" + month + "-" + day + "T" + hour + ":" + minute + ":" + second + "+08:00";
}

static bool isHexString(const String &s)
{
    // True iff s is non-empty, even-length, and all chars are [0-9A-Fa-f].
    // Used to guard against feeding plain ASCII (GSM 7bit text) into the UCS2 decoder.
    if (s.length() == 0 || (s.length() % 2) != 0)
        return false;
    for (size_t i = 0; i < s.length(); ++i)
    {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!ok)
            return false;
    }
    return true;
}

// ---------- UCS2 -> UTF-8 ----------

String decodeUCS2(String hex)
{
    // Remove whitespace/newlines
    String tmp;
    for (size_t i = 0; i < hex.length(); ++i)
    {
        char c = hex[i];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t')
            continue;
        tmp += c;
    }
    hex = tmp;
    hex.trim();

    // If it doesn't look like hex at all (e.g. module returned GSM 7bit ASCII
    // directly because CSCS != "UCS2"), pass it through unchanged.
    if (!isHexString(hex))
    {
        return hex;
    }

    auto hexVal = [](char c) -> int
    {
        c = (char)toupper((unsigned char)c);
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };

    auto hexByte = [&](int idx) -> int
    {
        if (idx + 1 >= (int)hex.length())
            return -1;
        int hi = hexVal(hex[idx]);
        int lo = hexVal(hex[idx + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        return (hi << 4) | lo;
    };

    String out;
    int len = hex.length();

    // If length is multiple of 4 -> decode as UTF-16BE (UCS2/UTF-16)
    if (len >= 4 && (len % 4) == 0)
    {
        for (int i = 0; i + 3 < len; i += 4)
        {
            int b1 = hexByte(i);
            int b2 = hexByte(i + 2);
            if (b1 < 0 || b2 < 0)
                break;
            uint16_t codeUnit = ((uint16_t)b1 << 8) | (uint16_t)b2;

            // Check for surrogate pair
            if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF && (i + 7) < len)
            {
                int b3 = hexByte(i + 4);
                int b4 = hexByte(i + 6);
                if (b3 >= 0 && b4 >= 0)
                {
                    uint16_t low = ((uint16_t)b3 << 8) | (uint16_t)b4;
                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        uint32_t high = codeUnit - 0xD800;
                        uint32_t lowpart = low - 0xDC00;
                        uint32_t codepoint = (high << 10) + lowpart + 0x10000;
                        out += (char)(0xF0 | ((codepoint >> 18) & 0x07));
                        out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        out += (char)(0x80 | (codepoint & 0x3F));
                        i += 4;
                        continue;
                    }
                }
            }

            uint16_t code = codeUnit;
            if (code <= 0x7F)
            {
                out += (char)code;
            }
            else if (code <= 0x7FF)
            {
                out += (char)(0xC0 | ((code >> 6) & 0x1F));
                out += (char)(0x80 | (code & 0x3F));
            }
            else
            {
                out += (char)(0xE0 | ((code >> 12) & 0x0F));
                out += (char)(0x80 | ((code >> 6) & 0x3F));
                out += (char)(0x80 | (code & 0x3F));
            }
        }
        return out;
    }

    // If hex length is even but not multiple of 4 -> treat as ASCII bytes in hex
    if ((len % 2) == 0)
    {
        for (int i = 0; i + 1 < len; i += 2)
        {
            int b = hexByte(i);
            if (b < 0)
                break;
            out += (char)b;
        }
        return out;
    }

    return out;
}

// ---------- AT+CMGR parsing ----------

bool parseCmgrBody(const String &raw, String &sender, String &timestamp, String &content)
{
    int header = raw.indexOf("+CMGR:");
    if (header == -1)
        return false;
    int headerEnd = raw.indexOf("\r\n", header);
    if (headerEnd == -1)
        return false;

    String headerLine = raw.substring(header, headerEnd);

    // Quote positions: 1,2 = status; 3,4 = sender; 5,6 = (alpha, often empty); 7,8 = timestamp
    int q1 = headerLine.indexOf('"');
    int q2 = headerLine.indexOf('"', q1 + 1);
    int q3 = headerLine.indexOf('"', q2 + 1);
    int q4 = headerLine.indexOf('"', q3 + 1);
    int q5 = headerLine.indexOf('"', q4 + 1);
    int q6 = headerLine.indexOf('"', q5 + 1);
    int q7 = headerLine.indexOf('"', q6 + 1);
    int q8 = headerLine.indexOf('"', q7 + 1);
    if (q8 == -1)
        return false;

    String senderHex = headerLine.substring(q3 + 1, q4);
    String ts = headerLine.substring(q7 + 1, q8);

    int bodyStart = headerEnd + 2;
    int bodyEnd = raw.indexOf("\r\nOK", bodyStart);
    if (bodyEnd == -1)
        bodyEnd = raw.length();

    String contentHex = raw.substring(bodyStart, bodyEnd);
    contentHex.trim();

    sender = decodeUCS2(senderHex);
    timestamp = ts;
    content = decodeUCS2(contentHex);
    return true;
}

// ---------- +CLIP URC parsing ----------

bool parseClipLine(const String &line, String &number)
{
    // Expected shapes (count of trailing fields varies by firmware):
    //   +CLIP: "+8613800138000",145,"",,"",0
    //   +CLIP: "13800138000",129
    //   +CLIP: "",128,"",,"",0           (withheld / anonymous)
    //
    // The only field we actually care about is the first quoted number.
    // A comma MUST follow the closing quote — a bare `+CLIP: "13800"`
    // with nothing after it is not a well-formed URC in the wild, and
    // we reject it so truly garbage input (no comma at all) is caught.
    if (!line.startsWith("+CLIP:"))
        return false;

    int q1 = line.indexOf('"');
    if (q1 == -1)
        return false;
    int q2 = line.indexOf('"', q1 + 1);
    if (q2 == -1)
        return false;

    // Require a comma after the closing quote — the field separator
    // between the number and the type. Without it, the line is
    // malformed (or a prefix of a partial read).
    int comma = line.indexOf(',', q2 + 1);
    if (comma == -1)
        return false;

    number = line.substring(q1 + 1, q2);
    return true;
}

// ---------- PDU mode parser (RFC-0002) ----------

namespace {

// Convert a single hex char to its nibble value; returns -1 if invalid.
int hexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// Decode a hex string to raw bytes. Returns false on malformed input.
bool hexToBytes(const String &hex, std::vector<uint8_t> &out)
{
    // Strip whitespace first; modems sometimes embed \r\n in long lines.
    std::string clean;
    clean.reserve(hex.length());
    for (unsigned int i = 0; i < hex.length(); ++i)
    {
        char c = hex[i];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t')
            continue;
        clean.push_back(c);
    }
    if ((clean.size() & 1) != 0)
        return false;
    out.clear();
    out.reserve(clean.size() / 2);
    for (size_t i = 0; i + 1 < clean.size(); i += 2)
    {
        int hi = hexNibble(clean[i]);
        int lo = hexNibble(clean[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

// GSM 7-bit default alphabet (3GPP TS 23.038). Entries are UTF-8
// strings so multi-byte characters (€, non-ASCII letters) work.
// Index = septet value (0..127). Values marked "" for the two codes
// that are never used in the default table (there aren't any — all
// 128 are assigned).
const char *const kGsmDefault[128] = {
    "@",    "\xC2\xA3", "$",    "\xC2\xA5", "\xC3\xA8", "\xC3\xA9", "\xC3\xB9", "\xC3\xAC",
    "\xC3\xB2", "\xC3\x87", "\n",   "\xC3\x98", "\xC3\xB8", "\r",   "\xC3\x85", "\xC3\xA5",
    "\xCE\x94", "_",    "\xCE\xA6", "\xCE\x93", "\xCE\x9B", "\xCE\xA9", "\xCE\xA0", "\xCE\xA8",
    "\xCE\xA3", "\xCE\x98", "\xCE\x9E", "",     "\xC3\x86", "\xC3\xA6", "\xC3\x9F", "\xC3\x89",
    " ",    "!",    "\"",   "#",    "\xC2\xA4", "%",    "&",    "'",
    "(",    ")",    "*",    "+",    ",",    "-",    ".",    "/",
    "0",    "1",    "2",    "3",    "4",    "5",    "6",    "7",
    "8",    "9",    ":",    ";",    "<",    "=",    ">",    "?",
    "\xC2\xA1", "A",    "B",    "C",    "D",    "E",    "F",    "G",
    "H",    "I",    "J",    "K",    "L",    "M",    "N",    "O",
    "P",    "Q",    "R",    "S",    "T",    "U",    "V",    "W",
    "X",    "Y",    "Z",    "\xC3\x84", "\xC3\x96", "\xC3\x91", "\xC3\x9C", "\xC2\xA7",
    "\xC2\xBF", "a",    "b",    "c",    "d",    "e",    "f",    "g",
    "h",    "i",    "j",    "k",    "l",    "m",    "n",    "o",
    "p",    "q",    "r",    "s",    "t",    "u",    "v",    "w",
    "x",    "y",    "z",    "\xC3\xA4", "\xC3\xB6", "\xC3\xB1", "\xC3\xBC", "\xC3\xA0",
};

// Extension table for GSM-7 after the 0x1B escape. Only a few entries
// are assigned; everything else falls back to a single space (as the
// spec recommends).
const char *gsmExtension(uint8_t c)
{
    switch (c)
    {
    case 0x0A: return "\x0C";       // form feed
    case 0x14: return "^";
    case 0x28: return "{";
    case 0x29: return "}";
    case 0x2F: return "\\";
    case 0x3C: return "[";
    case 0x3D: return "~";
    case 0x3E: return "]";
    case 0x40: return "|";
    case 0x65: return "\xE2\x82\xAC"; // €
    default:   return " ";
    }
}

// Unpack `numSeptets` septets from `data` starting at bit offset
// `bitOffset`. Result is a std::vector<uint8_t> of septet values
// (0..127).
std::vector<uint8_t> unpackSeptets(const uint8_t *data, size_t dataLen,
                                   size_t numSeptets, size_t bitOffset)
{
    std::vector<uint8_t> out;
    out.reserve(numSeptets);
    for (size_t i = 0; i < numSeptets; ++i)
    {
        size_t bit = bitOffset + i * 7;
        size_t byte = bit / 8;
        size_t shift = bit % 8;
        if (byte >= dataLen)
            break;
        uint16_t v = data[byte];
        if (byte + 1 < dataLen)
            v |= ((uint16_t)data[byte + 1]) << 8;
        uint8_t septet = (uint8_t)((v >> shift) & 0x7F);
        out.push_back(septet);
    }
    return out;
}

// Decode a sequence of GSM-7 septets (already unpacked) to UTF-8,
// handling the 0x1B extension escape.
String gsmSeptetsToUtf8(const std::vector<uint8_t> &septets)
{
    String out;
    for (size_t i = 0; i < septets.size(); ++i)
    {
        uint8_t s = septets[i];
        if (s == 0x1B)
        {
            if (i + 1 < septets.size())
            {
                ++i;
                out += gsmExtension(septets[i]);
            }
            // lone escape at end -> ignore
            continue;
        }
        if (s < 128)
        {
            out += kGsmDefault[s];
        }
    }
    return out;
}

// Decode a UCS-2 (UTF-16BE) byte buffer to UTF-8 String. Supports BMP
// and surrogate pairs. Malformed trailing bytes are dropped silently.
String ucs2BytesToUtf8(const uint8_t *data, size_t len)
{
    String out;
    size_t i = 0;
    while (i + 1 < len)
    {
        uint16_t code = ((uint16_t)data[i] << 8) | data[i + 1];
        i += 2;
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < len)
        {
            uint16_t low = ((uint16_t)data[i] << 8) | data[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                i += 2;
                uint32_t high = code - 0xD800;
                uint32_t lowpart = low - 0xDC00;
                uint32_t cp = (high << 10) + lowpart + 0x10000;
                out += (char)(0xF0 | ((cp >> 18) & 0x07));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
                continue;
            }
        }
        if (code <= 0x7F)
        {
            out += (char)code;
        }
        else if (code <= 0x7FF)
        {
            out += (char)(0xC0 | ((code >> 6) & 0x1F));
            out += (char)(0x80 | (code & 0x3F));
        }
        else
        {
            out += (char)(0xE0 | ((code >> 12) & 0x0F));
            out += (char)(0x80 | ((code >> 6) & 0x3F));
            out += (char)(0x80 | (code & 0x3F));
        }
    }
    return out;
}

// Decode a semi-octet swapped BCD address (the TP-OA digits).
// `len` is the number of digit nibbles to extract; each byte holds two
// digits with the low nibble being the first digit. Non-decimal
// nibbles (e.g. 'F' fill) are stopped-at.
String decodeBcdAddress(const uint8_t *data, size_t byteLen, size_t digitLen)
{
    String out;
    for (size_t i = 0; i < byteLen && out.length() < digitLen; ++i)
    {
        uint8_t b = data[i];
        uint8_t lo = b & 0x0F;
        uint8_t hi = (b >> 4) & 0x0F;
        if (lo <= 9)
            out += (char)('0' + lo);
        else
            break;
        if (out.length() >= digitLen)
            break;
        if (hi <= 9)
            out += (char)('0' + hi);
        else
            break;
    }
    return out;
}

// Decode the 7-byte SCTS timestamp to "yy/MM/dd,HH:mm:ss+zz" format.
// Each byte is semi-octet swapped BCD. The TZ byte encodes sign in
// the high bit of the HIGH nibble; value is in quarter-hours.
String decodeScts(const uint8_t *data)
{
    auto swap = [](uint8_t b) -> uint8_t {
        uint8_t lo = b & 0x0F;
        uint8_t hi = (b >> 4) & 0x0F;
        return (uint8_t)(lo * 10 + hi);
    };
    auto twoDigit = [](uint8_t v) -> String {
        String s;
        s += (char)('0' + (v / 10) % 10);
        s += (char)('0' + v % 10);
        return s;
    };

    uint8_t yy = swap(data[0]);
    uint8_t mo = swap(data[1]);
    uint8_t dd = swap(data[2]);
    uint8_t hh = swap(data[3]);
    uint8_t mi = swap(data[4]);
    uint8_t ss = swap(data[5]);

    // TZ: bit 3 (value 0x08) of the RAW high nibble is the sign; the
    // value is in quarter-hours. Mask the sign off before BCD swap.
    uint8_t tzRaw = data[6];
    uint8_t tzHiNibble = (tzRaw >> 4) & 0x0F;
    bool negative = (tzHiNibble & 0x08) != 0;
    uint8_t tzHi = tzHiNibble & 0x07;
    uint8_t tzLo = tzRaw & 0x0F;
    uint8_t tzQuarters = (uint8_t)(tzLo * 10 + tzHi);

    String out;
    out += twoDigit(yy);
    out += '/';
    out += twoDigit(mo);
    out += '/';
    out += twoDigit(dd);
    out += ',';
    out += twoDigit(hh);
    out += ':';
    out += twoDigit(mi);
    out += ':';
    out += twoDigit(ss);
    out += (negative ? '-' : '+');
    out += twoDigit(tzQuarters);
    return out;
}

} // anonymous namespace

bool parseSmsPdu(const String &hexPdu, SmsPdu &out)
{
    std::vector<uint8_t> buf;
    if (!hexToBytes(hexPdu, buf))
        return false;

    out = SmsPdu{};

    size_t p = 0;
    size_t n = buf.size();

    // ---------- SCA (Service Centre Address) ----------
    if (p >= n)
        return false;
    uint8_t scaLen = buf[p++];
    // SCA length is in BYTES (type-of-address + BCD digits).
    if (p + scaLen > n)
        return false;
    p += scaLen;

    // ---------- First octet (TP-MTI / TP-UDHI / flags) ----------
    if (p >= n)
        return false;
    uint8_t firstOctet = buf[p++];
    bool udhi = (firstOctet & 0x40) != 0;
    // We only handle SMS-DELIVER (TP-MTI = 00), but we don't hard-fail
    // on other types so an operator's odd PDU still gets handled best-
    // effort — the callers will just see an empty body.

    // ---------- TP-OA (Originating Address) ----------
    if (p >= n)
        return false;
    uint8_t oaDigits = buf[p++]; // in NIBBLES (digit count, NOT bytes)
    if (p >= n)
        return false;
    uint8_t oaToa = buf[p++];
    size_t oaBytes = (oaDigits + 1) / 2;
    if (p + oaBytes > n)
        return false;

    String senderDigits;
    bool oaIsAlphaNumeric = ((oaToa & 0x70) == 0x50);
    if (oaIsAlphaNumeric)
    {
        // Alphanumeric address: the digits field holds packed GSM 7-bit
        // septets. `oaDigits` is in semi-octet units, so the byte
        // length is (oaDigits+1)/2 and the septet count is
        // (byteLen * 8) / 7.
        size_t septetCount = (oaBytes * 8) / 7;
        std::vector<uint8_t> septets = unpackSeptets(buf.data() + p, oaBytes, septetCount, 0);
        senderDigits = gsmSeptetsToUtf8(septets);
    }
    else
    {
        senderDigits = decodeBcdAddress(buf.data() + p, oaBytes, oaDigits);
        // Mark international numbers with a leading '+'.
        if ((oaToa & 0x70) == 0x10)
        {
            senderDigits = String("+") + senderDigits;
        }
    }
    p += oaBytes;
    out.sender = senderDigits;

    // ---------- TP-PID ----------
    if (p >= n)
        return false;
    /* uint8_t pid = buf[p++]; */
    p++;

    // ---------- TP-DCS ----------
    if (p >= n)
        return false;
    uint8_t dcs = buf[p++];

    // Determine alphabet. Low 4 bits (for general data coding group 00xx):
    //   bits 3..2 == 00 -> GSM 7-bit
    //   bits 3..2 == 01 -> 8-bit
    //   bits 3..2 == 10 -> UCS-2
    // Other DCS groups (F0 = message waiting, etc.) we map by bits 3..2
    // as a best-effort heuristic.
    enum class Alpha { Gsm7, Eight, Ucs2 };
    Alpha alpha = Alpha::Gsm7;
    if ((dcs & 0xC0) == 0x00)
    {
        // General data coding indication
        uint8_t a = (dcs >> 2) & 0x03;
        if (a == 0) alpha = Alpha::Gsm7;
        else if (a == 1) alpha = Alpha::Eight;
        else if (a == 2) alpha = Alpha::Ucs2;
        else alpha = Alpha::Gsm7;  // reserved -> fallback
    }
    else if ((dcs & 0xF0) == 0xF0)
    {
        // Data coding / message class
        alpha = (dcs & 0x04) ? Alpha::Eight : Alpha::Gsm7;
    }
    else
    {
        // Message waiting indication groups etc — best-effort.
        if ((dcs & 0xF0) == 0xE0)
            alpha = Alpha::Ucs2;
        else
            alpha = Alpha::Gsm7;
    }

    // ---------- TP-SCTS (7 bytes) ----------
    if (p + 7 > n)
        return false;
    out.timestamp = decodeScts(buf.data() + p);
    p += 7;

    // ---------- TP-UDL + TP-UD ----------
    if (p >= n)
        return false;
    uint8_t udl = buf[p++];
    // `udl` is in SEPTETS for GSM-7 (even when UDH is present), and in
    // OCTETS for 8-bit / UCS-2.
    size_t udRemainingBytes = n - p;
    if (alpha == Alpha::Gsm7)
    {
        // For GSM-7, UDL is septet count. We don't strictly know how
        // many bytes that maps to without checking, but the PDU tells
        // us by total length: the remaining bytes are exactly the UD.
        // Nothing to do here.
    }
    else
    {
        if (udl > udRemainingBytes)
            return false;
        udRemainingBytes = udl;
    }

    // ---------- UDH (if TP-UDHI set) ----------
    size_t udhTotalOctets = 0;    // Length of UDH in octets including UDHL byte
    if (udhi)
    {
        if (udRemainingBytes < 1)
            return false;
        uint8_t udhl = buf[p];
        udhTotalOctets = (size_t)udhl + 1;
        if (udhTotalOctets > udRemainingBytes)
            return false;

        // Walk IEIs inside the UDH looking for concatenation IE.
        size_t ieCur = p + 1;
        size_t ieEnd = p + udhTotalOctets;
        while (ieCur + 1 < ieEnd)
        {
            uint8_t iei = buf[ieCur++];
            uint8_t iedl = buf[ieCur++];
            if (ieCur + iedl > ieEnd)
                break;
            if (iei == 0x00 && iedl == 3)
            {
                out.isConcatenated = true;
                out.concatRefNumber = buf[ieCur];
                out.concatTotalParts = buf[ieCur + 1];
                out.concatPartNumber = buf[ieCur + 2];
            }
            else if (iei == 0x08 && iedl == 4)
            {
                out.isConcatenated = true;
                out.concatRefNumber = (uint16_t)((buf[ieCur] << 8) | buf[ieCur + 1]);
                out.concatTotalParts = buf[ieCur + 2];
                out.concatPartNumber = buf[ieCur + 3];
            }
            ieCur += iedl;
        }
    }

    // ---------- Decode content ----------
    if (alpha == Alpha::Gsm7)
    {
        // Total bytes in UD = remaining buffer bytes.
        size_t udBytes = udRemainingBytes;
        if (udBytes > (n - p))
            udBytes = n - p;
        if (udhi)
        {
            // The UDH consumes `udhTotalOctets` octets at the start of
            // the UD. The first character starts at the next septet
            // boundary after the UDH — i.e. we need fill bits so that
            // (UDHoctets * 8 + fill) % 7 == 0.
            size_t udhBits = udhTotalOctets * 8;
            size_t fill = (7 - (udhBits % 7)) % 7;
            // Number of septets in UDH (including fill) that we skip:
            size_t headerSeptets = (udhBits + fill) / 7;
            if (headerSeptets > udl)
                return false;
            size_t bodySeptets = udl - headerSeptets;
            // Start at bit offset (udhTotalOctets*8 + fill)
            size_t bitOffset = udhBits + fill;
            // For unpacking we can pass the whole buffer with the
            // correct bit offset.
            std::vector<uint8_t> septets = unpackSeptets(buf.data() + p, udBytes, bodySeptets, bitOffset);
            out.content = gsmSeptetsToUtf8(septets);
        }
        else
        {
            std::vector<uint8_t> septets = unpackSeptets(buf.data() + p, udBytes, udl, 0);
            out.content = gsmSeptetsToUtf8(septets);
        }
    }
    else if (alpha == Alpha::Ucs2)
    {
        size_t udBytes = udRemainingBytes;
        if (udBytes > (n - p))
            udBytes = n - p;
        size_t bodyStart = udhi ? udhTotalOctets : 0;
        if (bodyStart > udBytes)
            return false;
        out.content = ucs2BytesToUtf8(buf.data() + p + bodyStart, udBytes - bodyStart);
    }
    else
    {
        // 8-bit data: pass through verbatim. Strip UDH if present.
        size_t udBytes = udRemainingBytes;
        if (udBytes > (n - p))
            udBytes = n - p;
        size_t bodyStart = udhi ? udhTotalOctets : 0;
        if (bodyStart > udBytes)
            return false;
        String content;
        for (size_t i = p + bodyStart; i < p + udBytes; ++i)
            content += (char)buf[i];
        out.content = content;
    }

    return true;
}

// ---------- PDU mode encoder (Unicode SMS TX) ----------

// Pack an array of 7-bit septets into bytes (GSM-7 packing).
// bitOffset: number of bits to skip at the start of the output buffer
// before writing the first septet.  For normal single-part PDUs this
// is 0.  For concat parts with a 6-byte UDH (48 bits), bitOffset=49
// aligns the first user-data septet to the next 7-bit boundary after
// the fill bit at bit 48.
//
// The first bitOffset bits of the output are zero-filled (the caller
// overwrites bytes 0..(UDHL) with the actual UDH bytes after calling
// this function).
//
// The guard `if (byteIdx + 1 < numBytes)` is unchanged from the
// original; with bitOffset=49 the high-byte write for septet 0 lands
// at out[7], which is always < numBytes for any non-empty septet list,
// so the guard fires correctly.
std::vector<uint8_t> packSeptets(const std::vector<uint8_t> &septets,
                                 int bitOffset)
{
    size_t bo = (bitOffset < 0) ? 0 : (size_t)bitOffset;
    size_t numBytes = (bo + septets.size() * 7 + 7) / 8;
    std::vector<uint8_t> out(numBytes, 0);
    for (size_t i = 0; i < septets.size(); ++i)
    {
        uint16_t val = (uint16_t)(septets[i] & 0x7F);
        size_t bitPos = bo + i * 7;
        size_t byteIdx = bitPos / 8;
        size_t bitOff = bitPos % 8;
        val <<= bitOff;
        out[byteIdx] |= (uint8_t)(val & 0xFF);
        if (byteIdx + 1 < numBytes)
            out[byteIdx + 1] |= (uint8_t)((val >> 8) & 0xFF);
    }
    return out;
}

namespace {

// Decode a UTF-8 String to a sequence of Unicode code points.
std::vector<uint32_t> utf8ToCodePoints(const String &s)
{
    std::vector<uint32_t> out;
    unsigned int i = 0;
    while (i < s.length())
    {
        uint8_t b = (uint8_t)s[i];
        uint32_t cp;
        int extra;
        if (b < 0x80)       { cp = b;          extra = 0; }
        else if (b < 0xC0)  { ++i; continue; }  // stray continuation byte
        else if (b < 0xE0)  { cp = b & 0x1F;   extra = 1; }
        else if (b < 0xF0)  { cp = b & 0x0F;   extra = 2; }
        else                { cp = b & 0x07;   extra = 3; }
        ++i;
        for (int j = 0; j < extra && i < s.length(); ++j, ++i)
            cp = (cp << 6) | ((uint8_t)s[i] & 0x3F);
        out.push_back(cp);
    }
    return out;
}

// Try to encode a single Unicode code point as GSM-7 septet(s).
// Writes into `out` (caller must provide space for at least 2 bytes).
// Returns the number of septets written (1 or 2), or 0 if the code
// point has no representation in the GSM 7-bit alphabet.
int encodeGsm7Char(uint32_t cp, uint8_t *out)
{
    // Fast path: printable ASCII 0x20-0x7E (most map directly)
    if (cp >= 0x20 && cp <= 0x7E)
    {
        switch (cp)
        {
        // Characters that DO NOT share their ASCII position in GSM-7
        case 0x24: out[0] = 0x02; return 1; // $ (GSM septet 0x02)
        case 0x40: out[0] = 0x00; return 1; // @ (GSM septet 0x00)
        case 0x5F: out[0] = 0x11; return 1; // _ (GSM septet 0x11)
        case 0x60: return 0;                 // ` not in GSM-7
        // Extension table characters (2 septets: ESC + code)
        case 0x5B: out[0] = 0x1B; out[1] = 0x3C; return 2; // [
        case 0x5C: out[0] = 0x1B; out[1] = 0x2F; return 2; // backslash
        case 0x5D: out[0] = 0x1B; out[1] = 0x3E; return 2; // ]
        case 0x5E: out[0] = 0x1B; out[1] = 0x14; return 2; // ^
        case 0x7B: out[0] = 0x1B; out[1] = 0x28; return 2; // {
        case 0x7C: out[0] = 0x1B; out[1] = 0x40; return 2; // |
        case 0x7D: out[0] = 0x1B; out[1] = 0x29; return 2; // }
        case 0x7E: out[0] = 0x1B; out[1] = 0x3D; return 2; // ~
        default:
            out[0] = (uint8_t)cp;            // direct mapping
            return 1;
        }
    }
    // Control characters
    if (cp == 0x0A) { out[0] = 0x0A; return 1; } // LF
    if (cp == 0x0D) { out[0] = 0x0D; return 1; } // CR
    if (cp == 0x0C) { out[0] = 0x1B; out[1] = 0x0A; return 2; } // form feed
    // Non-ASCII characters in the GSM-7 basic table
    switch (cp)
    {
    case 0x00A1: out[0] = 0x40; return 1; // ¡
    case 0x00A3: out[0] = 0x01; return 1; // £
    case 0x00A4: out[0] = 0x24; return 1; // ¤
    case 0x00A5: out[0] = 0x03; return 1; // ¥
    case 0x00A7: out[0] = 0x5F; return 1; // §
    case 0x00BF: out[0] = 0x60; return 1; // ¿
    case 0x00C4: out[0] = 0x5B; return 1; // Ä
    case 0x00C5: out[0] = 0x0E; return 1; // Å
    case 0x00C6: out[0] = 0x1C; return 1; // Æ
    case 0x00C7: out[0] = 0x09; return 1; // Ç
    case 0x00C9: out[0] = 0x1F; return 1; // É
    case 0x00D1: out[0] = 0x5D; return 1; // Ñ
    case 0x00D6: out[0] = 0x5C; return 1; // Ö
    case 0x00D8: out[0] = 0x0B; return 1; // Ø
    case 0x00DC: out[0] = 0x5E; return 1; // Ü
    case 0x00DF: out[0] = 0x1E; return 1; // ß
    case 0x00E0: out[0] = 0x7F; return 1; // à
    case 0x00E4: out[0] = 0x7B; return 1; // ä
    case 0x00E5: out[0] = 0x0F; return 1; // å
    case 0x00E6: out[0] = 0x1D; return 1; // æ
    case 0x00E8: out[0] = 0x04; return 1; // è
    case 0x00E9: out[0] = 0x05; return 1; // é
    case 0x00EC: out[0] = 0x07; return 1; // ì
    case 0x00F1: out[0] = 0x7D; return 1; // ñ
    case 0x00F2: out[0] = 0x08; return 1; // ò
    case 0x00F6: out[0] = 0x7C; return 1; // ö
    case 0x00F8: out[0] = 0x0C; return 1; // ø
    case 0x00F9: out[0] = 0x06; return 1; // ù
    case 0x00FC: out[0] = 0x7E; return 1; // ü
    // Greek letters in GSM-7 basic table
    case 0x0393: out[0] = 0x13; return 1; // Γ
    case 0x0394: out[0] = 0x10; return 1; // Δ
    case 0x0398: out[0] = 0x19; return 1; // Θ
    case 0x039B: out[0] = 0x14; return 1; // Λ
    case 0x039E: out[0] = 0x1A; return 1; // Ξ
    case 0x03A0: out[0] = 0x16; return 1; // Π
    case 0x03A3: out[0] = 0x18; return 1; // Σ
    case 0x03A6: out[0] = 0x12; return 1; // Φ
    case 0x03A8: out[0] = 0x17; return 1; // Ψ
    case 0x03A9: out[0] = 0x15; return 1; // Ω
    // Euro sign (extension table)
    case 0x20AC: out[0] = 0x1B; out[1] = 0x65; return 2; // €
    default: return 0;
    }
}

// Encode Unicode code points to UTF-16BE (UCS-2 with surrogate pair
// support for supplementary characters).
std::vector<uint8_t> codePointsToUtf16BE(const std::vector<uint32_t> &cps)
{
    std::vector<uint8_t> out;
    out.reserve(cps.size() * 2);
    for (uint32_t cp : cps)
    {
        if (cp <= 0xFFFF)
        {
            out.push_back((uint8_t)(cp >> 8));
            out.push_back((uint8_t)(cp & 0xFF));
        }
        else if (cp <= 0x10FFFF)
        {
            uint32_t adj = cp - 0x10000;
            uint16_t hi = (uint16_t)(0xD800 + (adj >> 10));
            uint16_t lo = (uint16_t)(0xDC00 + (adj & 0x3FF));
            out.push_back((uint8_t)(hi >> 8));
            out.push_back((uint8_t)(hi & 0xFF));
            out.push_back((uint8_t)(lo >> 8));
            out.push_back((uint8_t)(lo & 0xFF));
        }
    }
    return out;
}

// Encode a phone number as a TP-DA (destination address) field:
//   [digit-count] [type-of-address] [BCD digits, semi-octet swapped]
void encodeBcdPhone(const String &phone, std::vector<uint8_t> &out)
{
    String digits;
    bool international = false;
    for (unsigned int i = 0; i < phone.length(); ++i)
    {
        char c = phone[i];
        if (c == '+')
            international = true;
        else if (c >= '0' && c <= '9')
            digits += c;
    }
    out.push_back((uint8_t)digits.length());
    out.push_back(international ? (uint8_t)0x91 : (uint8_t)0x81);
    for (unsigned int i = 0; i < digits.length(); i += 2)
    {
        uint8_t lo = (uint8_t)(digits[i] - '0');
        uint8_t hi = (i + 1 < digits.length())
                         ? (uint8_t)(digits[i + 1] - '0')
                         : (uint8_t)0x0F;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
}

static const char kHexChars[] = "0123456789ABCDEF";

String bytesToHex(const std::vector<uint8_t> &data)
{
    String out;
    for (uint8_t b : data)
    {
        out += kHexChars[(b >> 4) & 0x0F];
        out += kHexChars[b & 0x0F];
    }
    return out;
}

} // anonymous namespace

bool isGsm7Compatible(const String &s)
{
    auto cps = utf8ToCodePoints(s);
    uint8_t buf[2];
    for (uint32_t cp : cps)
    {
        if (encodeGsm7Char(cp, buf) == 0)
            return false;
    }
    return true;
}

std::vector<SmsSubmitPdu> buildSmsSubmitPduMulti(const String &phone,
                                                  const String &body,
                                                  int maxParts,
                                                  bool requestStatusReport)
{
    std::vector<SmsSubmitPdu> result;

    if (phone.length() == 0 || body.length() == 0)
        return result; // empty = error

    auto cps = utf8ToCodePoints(body);

    // --- Try GSM-7 first ---
    std::vector<uint8_t> gsm7Septets;
    bool canGsm7 = true;
    for (uint32_t cp : cps)
    {
        uint8_t buf[2];
        int n = encodeGsm7Char(cp, buf);
        if (n == 0) { canGsm7 = false; break; }
        for (int j = 0; j < n; ++j)
            gsm7Septets.push_back(buf[j]);
    }

    if (canGsm7)
    {
        // Single-part: body fits in one non-concatenated SMS (no UDH).
        if (gsm7Septets.size() <= 160)
        {
            auto packed = packSeptets(gsm7Septets);

            std::vector<uint8_t> pdu;
            pdu.push_back(0x00); // SCA: use default SMSC
            // SMS-SUBMIT first octet: 0x01 = basic; 0x21 = with TP-SRR
            pdu.push_back(requestStatusReport ? (uint8_t)0x21 : (uint8_t)0x01);
            pdu.push_back(0x00); // TP-MR: modem assigns
            encodeBcdPhone(phone, pdu);
            pdu.push_back(0x00); // TP-PID
            pdu.push_back(0x00); // TP-DCS: GSM 7-bit
            pdu.push_back((uint8_t)gsm7Septets.size()); // TP-UDL (septets)
            pdu.insert(pdu.end(), packed.begin(), packed.end());

            SmsSubmitPdu p;
            p.tpduLen = (int)pdu.size() - 1;
            p.hex = bytesToHex(pdu);
            result.push_back(p);
            return result;
        }

        // Multi-part GSM-7: split into 153-septet chunks with ESC-safe
        // boundary check.  UDH = 6 bytes = 48 bits; 1 fill bit brings
        // the first septet to bit offset 49 (7 header septets total).
        //
        // Static counter for concat reference number; incremented only
        // when multi-part is actually needed so single-part messages
        // do not consume reference numbers.
        static uint8_t s_concatRef = 0;

        // Build the list of per-part septet slices first so we know
        // the total part count before constructing PDU headers.
        std::vector<std::vector<uint8_t>> parts;
        size_t pos = 0;
        while (pos < gsm7Septets.size())
        {
            size_t remaining = gsm7Septets.size() - pos;
            size_t chunkSize = (remaining <= 153) ? remaining : 153;

            // ESC-safe split: if the last septet of the proposed chunk
            // is 0x1B (ESC), the extension char would be orphaned in
            // the next part -- trim.  If the second-to-last is 0x1B,
            // the lone ESC would be the last septet of this part --
            // also trim.  Both checks apply to the boundaries of the
            // proposed chunk (not the full array).
            if (chunkSize == 153 && remaining > 153)
            {
                if (gsm7Septets[pos + 152] == 0x1B)
                    chunkSize = 152;
                else if (gsm7Septets[pos + 151] == 0x1B)
                    chunkSize = 151;
            }

            parts.push_back(std::vector<uint8_t>(
                gsm7Septets.begin() + pos,
                gsm7Septets.begin() + pos + chunkSize));
            pos += chunkSize;
        }

        int totalParts = (int)parts.size();
        if (totalParts > maxParts)
            return result; // empty = error (too long)

        uint8_t ref = s_concatRef++;

        for (int partNum = 1; partNum <= totalParts; ++partNum)
        {
            const auto &chunk = parts[partNum - 1];

            // UDH: UDHL=05 IEI=00 IEDL=03 ref total seq (6 bytes)
            // Pack septets with bitOffset=49 so the first user-data
            // septet starts at bit 49 (after 48 UDH bits + 1 fill bit).
            // packSeptets returns a buffer whose first 6 bytes are
            // zero-filled; we overwrite them with the actual UDH bytes.
            auto packed = packSeptets(chunk, 49);
            // Ensure the buffer is large enough for the UDH bytes.
            // With bitOffset=49 the buffer already starts at byte 0,
            // so indices 0-5 are zero and safe to overwrite.
            packed[0] = 0x05; // UDHL
            packed[1] = 0x00; // IEI (concat, 8-bit ref)
            packed[2] = 0x03; // IEDL
            packed[3] = ref;
            packed[4] = (uint8_t)totalParts;
            packed[5] = (uint8_t)partNum;

            // TP-UDL for GSM-7 with UDH = header septets + payload septets.
            // 6 UDH bytes = 48 bits; fill 1 bit to reach 49-bit boundary;
            // total header = 49 bits = 7 septets.
            uint8_t udl = (uint8_t)(7 + chunk.size());

            std::vector<uint8_t> pdu;
            pdu.push_back(0x00); // SCA
            // SMS-SUBMIT | UDHI: 0x41 = basic concat; 0x61 = concat + TP-SRR
            pdu.push_back(requestStatusReport ? (uint8_t)0x61 : (uint8_t)0x41);
            pdu.push_back(0x00); // TP-MR
            encodeBcdPhone(phone, pdu);
            pdu.push_back(0x00); // TP-PID
            pdu.push_back(0x00); // TP-DCS: GSM 7-bit
            pdu.push_back(udl);
            pdu.insert(pdu.end(), packed.begin(), packed.end());

            SmsSubmitPdu p;
            p.tpduLen = (int)pdu.size() - 1;
            p.hex = bytesToHex(pdu);
            result.push_back(p);
        }
        return result;
    }

    // --- Fall back to UCS-2 / UTF-16BE ---
    auto ucs2 = codePointsToUtf16BE(cps);

    // Single-part UCS-2: fits in 140 octets (70 code units).
    if (ucs2.size() <= 140)
    {
        std::vector<uint8_t> pdu;
        pdu.push_back(0x00); // SCA
        // SMS-SUBMIT first octet: 0x01 = basic; 0x21 = with TP-SRR
        pdu.push_back(requestStatusReport ? (uint8_t)0x21 : (uint8_t)0x01);
        pdu.push_back(0x00); // TP-MR
        encodeBcdPhone(phone, pdu);
        pdu.push_back(0x00); // TP-PID
        pdu.push_back(0x08); // TP-DCS: UCS-2
        pdu.push_back((uint8_t)ucs2.size()); // TP-UDL (octets)
        pdu.insert(pdu.end(), ucs2.begin(), ucs2.end());

        SmsSubmitPdu p;
        p.tpduLen = (int)pdu.size() - 1;
        p.hex = bytesToHex(pdu);
        result.push_back(p);
        return result;
    }

    // Multi-part UCS-2: split into 134-octet (67 code-unit) chunks.
    // No bit-alignment needed; UDH is simply prepended (6 bytes).
    // Split must not land between surrogate pairs.
    static uint8_t s_concatRefUcs2 = 0;

    std::vector<std::vector<uint8_t>> ucs2Parts;
    size_t pos = 0;
    while (pos < ucs2.size())
    {
        size_t remaining = ucs2.size() - pos;
        size_t chunkBytes = (remaining <= 134) ? remaining : 134;

        // Surrogate-safe split: if the 67th UTF-16BE code unit would
        // be a high surrogate (0xD800-0xDBFF), trim to 66 code units
        // so the surrogate pair is not split across parts.
        if (chunkBytes == 134 && remaining > 134)
        {
            uint8_t hiHi = ucs2[pos + 132];
            uint8_t hiLo = ucs2[pos + 133];
            uint16_t cu = (uint16_t)((hiHi << 8) | hiLo);
            if (cu >= 0xD800 && cu <= 0xDBFF)
                chunkBytes = 132; // drop the high surrogate to next part
        }

        ucs2Parts.push_back(std::vector<uint8_t>(
            ucs2.begin() + pos, ucs2.begin() + pos + chunkBytes));
        pos += chunkBytes;
    }

    int totalParts = (int)ucs2Parts.size();
    if (totalParts > maxParts)
        return result; // empty = error (too long)

    uint8_t ref = s_concatRefUcs2++;

    for (int partNum = 1; partNum <= totalParts; ++partNum)
    {
        const auto &chunk = ucs2Parts[partNum - 1];

        // UDH: 6 bytes prepended to UTF-16BE payload.
        // TP-UDL = 6 (UDH bytes) + payload bytes.
        std::vector<uint8_t> ud;
        ud.push_back(0x05); // UDHL
        ud.push_back(0x00); // IEI
        ud.push_back(0x03); // IEDL
        ud.push_back(ref);
        ud.push_back((uint8_t)totalParts);
        ud.push_back((uint8_t)partNum);
        ud.insert(ud.end(), chunk.begin(), chunk.end());

        uint8_t udl = (uint8_t)(6 + chunk.size()); // UDH bytes + payload bytes

        std::vector<uint8_t> pdu;
        pdu.push_back(0x00); // SCA
        // SMS-SUBMIT | UDHI: 0x41 = basic concat; 0x61 = concat + TP-SRR
        pdu.push_back(requestStatusReport ? (uint8_t)0x61 : (uint8_t)0x41);
        pdu.push_back(0x00); // TP-MR
        encodeBcdPhone(phone, pdu);
        pdu.push_back(0x00); // TP-PID
        pdu.push_back(0x08); // TP-DCS: UCS-2
        pdu.push_back(udl);
        pdu.insert(pdu.end(), ud.begin(), ud.end());

        SmsSubmitPdu p;
        p.tpduLen = (int)pdu.size() - 1;
        p.hex = bytesToHex(pdu);
        result.push_back(p);
    }
    return result;
}

// Backward-compatible single-PDU overload.  Delegates to
// buildSmsSubmitPduMulti and returns false if the result has more than
// one part (body too long for a single non-concatenated SMS) or if the
// multi function returns an empty vector (error).  Production callers
// should use buildSmsSubmitPduMulti directly.
bool buildSmsSubmitPdu(const String &phone, const String &body,
                       SmsSubmitPdu &out)
{
    auto pdus = buildSmsSubmitPduMulti(phone, body);
    if (pdus.empty() || pdus.size() > 1)
        return false;
    out = pdus[0];
    return true;
}

// ---------- SMS-STATUS-REPORT PDU parser (RFC-0011) ----------

// Helper: convert a BCD semi-octet timestamp (7 bytes) to a human-readable
// string "YY/MM/DD,HH:MM:SS+TZ". This is the same SCTS format used in
// SMS-DELIVER PDUs; we reuse it here for both TP-SCTS and TP-DT fields.
static String parseScts(const std::vector<uint8_t> &raw, size_t offset)
{
    if (offset + 7 > raw.size())
        return String();

    // Each byte is a pair of semi-octets in reversed nibble order.
    auto swapNibbles = [](uint8_t b) -> uint8_t {
        return (uint8_t)(((b & 0x0F) * 10) + ((b >> 4) & 0x0F));
    };

    char buf[24];
    snprintf(buf, sizeof(buf), "%02d/%02d/%02d,%02d:%02d:%02d+%02d",
             swapNibbles(raw[offset + 0]), swapNibbles(raw[offset + 1]),
             swapNibbles(raw[offset + 2]), swapNibbles(raw[offset + 3]),
             swapNibbles(raw[offset + 4]), swapNibbles(raw[offset + 5]),
             swapNibbles(raw[offset + 6]) & 0x7F); // mask sign bit for TZ
    return String(buf);
}

// Map TP-ST value to a human-readable string (3GPP TS 23.040 §9.2.3.15).
static String statusText(uint8_t st)
{
    // Helper: format one byte as two hex digits without using Arduino's
    // radix String constructor (which is absent from the native test stub).
    auto hexByte = [](uint8_t v) -> String {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", (unsigned)v);
        return String(buf);
    };

    if (st == 0x00) return String("delivered");
    if (st == 0x01) return String("forwarded, unconfirmed");
    if (st == 0x02) return String("replaced");
    // 0x20-0x2F: temporary, SC still trying
    if (st >= 0x20 && st <= 0x2F)
    {
        switch (st)
        {
        case 0x20: return String("temporary failure, still trying (congestion)");
        case 0x21: return String("temporary failure, still trying (SME busy)");
        case 0x22: return String("temporary failure, still trying (no response from SME)");
        case 0x23: return String("temporary failure, still trying (service rejected)");
        default:   break;
        }
        return String("temporary failure, still trying (0x") + hexByte(st) + ")";
    }
    // 0x40-0x4F: permanent error, SC stopped trying
    if (st >= 0x40 && st <= 0x4F)
    {
        switch (st)
        {
        case 0x40: return String("permanent failure (remote procedure error)");
        case 0x41: return String("permanent failure (incompatible destination)");
        case 0x42: return String("permanent failure (connection rejected by SME)");
        case 0x43: return String("permanent failure (not obtainable)");
        case 0x44: return String("permanent failure (quality unavailable)");
        case 0x45: return String("permanent failure (no interworking available)");
        default:   break;
        }
        return String("permanent failure (0x") + hexByte(st) + ")";
    }
    // 0x60-0x6F: temporary error, SC stopped trying
    if (st >= 0x60 && st <= 0x6F)
    {
        switch (st)
        {
        case 0x60: return String("temporary failure, stopped trying (congestion)");
        case 0x61: return String("temporary failure, stopped trying (SME busy)");
        default:   break;
        }
        return String("temporary failure, stopped trying (0x") + hexByte(st) + ")";
    }
    return String("unknown status 0x") + hexByte(st);
}

bool parseStatusReportPdu(const String &hexPdu, StatusReport &out)
{
    // Convert hex string to raw bytes.
    if (hexPdu.length() < 2 || (hexPdu.length() % 2) != 0)
        return false;

    std::vector<uint8_t> raw;
    raw.reserve(hexPdu.length() / 2);
    for (unsigned int i = 0; i < hexPdu.length(); i += 2)
    {
        uint8_t hi = hexNibble(hexPdu[i]);
        uint8_t lo = hexNibble(hexPdu[i + 1]);
        raw.push_back((uint8_t)((hi << 4) | lo));
    }

    size_t pos = 0;

    // SCA (Service Centre Address): first byte is length (in octets).
    if (pos >= raw.size())
        return false;
    uint8_t scaLen = raw[pos++];
    pos += scaLen; // skip SCA bytes

    // First octet (TP-MTI + flags).
    if (pos >= raw.size())
        return false;
    uint8_t firstOctet = raw[pos++];

    // TP-MTI must be 0b10 (binary 10 = decimal 2) for SMS-STATUS-REPORT.
    if ((firstOctet & 0x03) != 0x02)
        return false;

    // TP-MR: Message Reference (1 byte).
    if (pos >= raw.size())
        return false;
    out.messageRef = raw[pos++];

    // TP-RA: Recipient Address (same BCD encoding as TP-OA).
    if (pos >= raw.size())
        return false;
    uint8_t raDigits = raw[pos++]; // digit count
    if (pos >= raw.size())
        return false;
    uint8_t raToa = raw[pos++]; // type-of-address
    uint8_t raBytes = (raDigits + 1) / 2;
    if (pos + raBytes > raw.size())
        return false;

    // Decode BCD phone digits.
    String phone;
    bool international = (raToa & 0x70) == 0x10;
    if (international)
        phone += "+";
    for (uint8_t b = 0; b < raBytes; ++b)
    {
        uint8_t byte = raw[pos + b];
        uint8_t d1 = byte & 0x0F;
        uint8_t d2 = (byte >> 4) & 0x0F;
        phone += (char)('0' + d1);
        if (b * 2 + 1 < raDigits)
            phone += (char)('0' + d2);
    }
    out.recipient = phone;
    pos += raBytes;

    // TP-SCTS: Service Centre Time Stamp (7 bytes).
    if (pos + 7 > raw.size())
        return false;
    out.scTimestamp = parseScts(raw, pos);
    pos += 7;

    // TP-DT: Discharge Time (7 bytes).
    if (pos + 7 > raw.size())
        return false;
    out.dischargeTime = parseScts(raw, pos);
    pos += 7;

    // TP-ST: Status (1 byte).
    if (pos >= raw.size())
        return false;
    out.status    = raw[pos++];
    out.delivered = (out.status == 0x00);
    out.statusText = statusText(out.status);

    return true;
}

// RFC-0037: part count without building full PDUs.
// Reuses buildSmsSubmitPduMulti for exact encoding selection.
int countSmsParts(const String &body, int maxParts)
{
    auto pdus = buildSmsSubmitPduMulti(String("+1"), body, maxParts);
    return (int)pdus.size();
}

} // namespace sms_codec
