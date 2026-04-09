#include "sms_codec.h"

#include <stdint.h>
#include <stddef.h>
#include <ctype.h>

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

} // namespace sms_codec
