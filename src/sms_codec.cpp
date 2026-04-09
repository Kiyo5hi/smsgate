#include "sms_codec.h"

#include <stdint.h>
#include <stddef.h>
#include <ctype.h>

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

} // namespace sms_codec
