#pragma once

// Hand-rolled PDU builder helpers for the native tests. Only used by
// the host test binary — NOT part of the firmware build.
//
// This lets us construct SMS-DELIVER PDUs programmatically and feed
// them to both the parser tests (test_sms_pdu.cpp) and the handler
// tests (test_sms_handler_pdu.cpp) without duplicating the builder
// across files.

#include <Arduino.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pdu_test {

inline String bytesToHex(const std::vector<uint8_t> &bytes)
{
    static const char *kHex = "0123456789ABCDEF";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes)
    {
        s.push_back(kHex[(b >> 4) & 0x0F]);
        s.push_back(kHex[b & 0x0F]);
    }
    return String(s);
}

// Pack a stream of 7-bit septets into octets, starting at the given
// bit offset. The bits below `bitOffset` are left at zero — the caller
// is responsible for filling in UDH bytes there afterwards.
inline std::vector<uint8_t> packSeptets(const std::vector<uint8_t> &septets, size_t bitOffset)
{
    size_t totalBits = bitOffset + septets.size() * 7;
    size_t totalBytes = (totalBits + 7) / 8;
    std::vector<uint8_t> out(totalBytes, 0);
    for (size_t i = 0; i < septets.size(); ++i)
    {
        size_t bit = bitOffset + i * 7;
        size_t byte = bit / 8;
        size_t shift = bit % 8;
        uint16_t v = (uint16_t)(septets[i] & 0x7F) << shift;
        out[byte] |= (uint8_t)(v & 0xFF);
        if (byte + 1 < out.size())
            out[byte + 1] |= (uint8_t)((v >> 8) & 0xFF);
    }
    return out;
}

// GSM-7 septet stream from ASCII (each character must map 1:1 to the
// default alphabet; the helper does NOT try to do locale escape
// sequences — tests should stick to plain ASCII bodies).
inline std::vector<uint8_t> gsm7FromAscii(const char *s)
{
    std::vector<uint8_t> out;
    for (const char *p = s; *p; ++p)
        out.push_back((uint8_t)(*p & 0x7F));
    return out;
}

// Semi-octet swap encoder for TP-OA / TP-DA phone numbers. Type of
// address is hard-coded to 0x91 (international). Returns the full
// address field: <length-in-digits><type><swapped-digits>.
inline std::vector<uint8_t> encodeTpOa(const char *digits)
{
    std::vector<uint8_t> body;
    size_t nDigits = 0;
    for (const char *p = digits; *p; ++p)
        if (*p >= '0' && *p <= '9')
            nDigits++;

    uint8_t cur = 0xF0;
    bool low = true;
    for (const char *p = digits; *p; ++p)
    {
        if (*p < '0' || *p > '9')
            continue;
        uint8_t d = (uint8_t)(*p - '0');
        if (low)
        {
            cur = (uint8_t)(0xF0 | d);
            low = false;
        }
        else
        {
            cur = (uint8_t)((cur & 0x0F) | (d << 4));
            body.push_back(cur);
            cur = 0xF0;
            low = true;
        }
    }
    if (!low)
        body.push_back(cur);

    std::vector<uint8_t> out;
    out.push_back((uint8_t)nDigits);
    out.push_back(0x91);
    for (auto b : body)
        out.push_back(b);
    return out;
}

// Fixed test timestamp: 24/01/15 10:30:45 +32 (i.e. GMT+8 in quarter
// hours).
inline std::vector<uint8_t> encodeScts()
{
    std::vector<uint8_t> out;
    auto enc = [](uint8_t v) -> uint8_t {
        uint8_t lo = (uint8_t)(v / 10);
        uint8_t hi = (uint8_t)(v % 10);
        return (uint8_t)((hi << 4) | lo);
    };
    out.push_back(enc(24));
    out.push_back(enc(1));
    out.push_back(enc(15));
    out.push_back(enc(10));
    out.push_back(enc(30));
    out.push_back(enc(45));
    out.push_back(enc(32));
    return out;
}

// UTF-8 -> UTF-16BE byte stream (no BOM). Supports BMP + astral
// (surrogate pair output).
inline std::vector<uint8_t> utf8ToUtf16Be(const char *utf8)
{
    std::vector<uint8_t> out;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p)
    {
        uint32_t cp = 0;
        if (*p < 0x80)
        {
            cp = *p++;
        }
        else if ((*p & 0xE0) == 0xC0)
        {
            cp = (uint32_t)(*p & 0x1F) << 6;
            p++;
            cp |= (uint32_t)(*p++ & 0x3F);
        }
        else if ((*p & 0xF0) == 0xE0)
        {
            cp = (uint32_t)(*p & 0x0F) << 12;
            p++;
            cp |= (uint32_t)(*p & 0x3F) << 6;
            p++;
            cp |= (uint32_t)(*p++ & 0x3F);
        }
        else if ((*p & 0xF8) == 0xF0)
        {
            cp = (uint32_t)(*p & 0x07) << 18;
            p++;
            cp |= (uint32_t)(*p & 0x3F) << 12;
            p++;
            cp |= (uint32_t)(*p & 0x3F) << 6;
            p++;
            cp |= (uint32_t)(*p++ & 0x3F);
        }
        else
        {
            p++;
            continue;
        }
        if (cp < 0x10000)
        {
            out.push_back((uint8_t)(cp >> 8));
            out.push_back((uint8_t)(cp & 0xFF));
        }
        else
        {
            uint32_t v = cp - 0x10000;
            uint16_t high = (uint16_t)(0xD800 | (v >> 10));
            uint16_t low = (uint16_t)(0xDC00 | (v & 0x3FF));
            out.push_back((uint8_t)(high >> 8));
            out.push_back((uint8_t)(high & 0xFF));
            out.push_back((uint8_t)(low >> 8));
            out.push_back((uint8_t)(low & 0xFF));
        }
    }
    return out;
}

struct PduBuildOpts
{
    const char *sender = "13800138000";
    uint8_t dcs = 0x00;         // 0x00 GSM-7, 0x08 UCS-2
    std::vector<uint8_t> bodyBytes; // septets (GSM-7) or UTF-16BE bytes (UCS-2)
    bool addConcatUdh = false;
    bool udh16bit = false;      // IEI 0x00 vs 0x08
    uint16_t concatRef = 0;
    uint8_t concatTotal = 0;
    uint8_t concatPart = 0;
};

inline String buildPduHex(const PduBuildOpts &opts)
{
    std::vector<uint8_t> pdu;
    // SCA = 0 length
    pdu.push_back(0x00);

    // First octet: TP-MTI=00, TP-UDHI bit 6
    uint8_t firstOct = 0x00;
    if (opts.addConcatUdh)
        firstOct |= 0x40;
    pdu.push_back(firstOct);

    auto tpoa = encodeTpOa(opts.sender);
    for (auto b : tpoa)
        pdu.push_back(b);

    pdu.push_back(0x00); // TP-PID
    pdu.push_back(opts.dcs); // TP-DCS
    auto scts = encodeScts();
    for (auto b : scts)
        pdu.push_back(b);

    std::vector<uint8_t> udh;
    if (opts.addConcatUdh)
    {
        if (!opts.udh16bit)
        {
            udh.push_back(0x05);
            udh.push_back(0x00);
            udh.push_back(0x03);
            udh.push_back((uint8_t)(opts.concatRef & 0xFF));
            udh.push_back(opts.concatTotal);
            udh.push_back(opts.concatPart);
        }
        else
        {
            udh.push_back(0x06);
            udh.push_back(0x08);
            udh.push_back(0x04);
            udh.push_back((uint8_t)((opts.concatRef >> 8) & 0xFF));
            udh.push_back((uint8_t)(opts.concatRef & 0xFF));
            udh.push_back(opts.concatTotal);
            udh.push_back(opts.concatPart);
        }
    }

    if (opts.dcs == 0x00)
    {
        // GSM-7
        size_t udhOctets = udh.size();
        size_t udhBits = udhOctets * 8;
        size_t fill = (7 - (udhBits % 7)) % 7;
        size_t headerSeptets = (udhBits + fill) / 7;
        size_t totalSeptets = headerSeptets + opts.bodyBytes.size();

        pdu.push_back((uint8_t)totalSeptets);

        std::vector<uint8_t> packed = packSeptets(opts.bodyBytes, udhBits + fill);

        // Replace the leading `udhOctets` zero bytes with the raw UDH.
        // The fill bits live in the low bits of byte `udhOctets` and
        // are already zero.
        for (auto b : udh)
            pdu.push_back(b);
        for (size_t i = udhOctets; i < packed.size(); ++i)
            pdu.push_back(packed[i]);
    }
    else
    {
        // UCS-2 or 8-bit. UDL = octets, including UDH.
        size_t udlOctets = udh.size() + opts.bodyBytes.size();
        pdu.push_back((uint8_t)udlOctets);
        for (auto b : udh)
            pdu.push_back(b);
        for (auto b : opts.bodyBytes)
            pdu.push_back(b);
    }

    return bytesToHex(pdu);
}

// Wrap a raw PDU hex string in a realistic +CMGR response envelope.
// Matches the shape the modem emits in PDU mode: a +CMGR header line
// carrying <status>,<alpha>,<length>, then the hex PDU, then OK.
inline String wrapInCmgrResponse(const String &hex)
{
    return String("+CMGR: 0,,") + String((int)(hex.length() / 2)) + "\r\n" +
           hex + "\r\n\r\nOK\r\n";
}

} // namespace pdu_test
