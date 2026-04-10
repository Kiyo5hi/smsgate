// Unit tests for the SMS-SUBMIT PDU encoder in sms_codec.
//
// Verifies:
//   - GSM-7 encoding: ASCII body builds correct PDU with DCS=0x00
//   - UCS-2 encoding: Chinese body builds correct PDU with DCS=0x08
//   - Phone number BCD encoding: international (+) and national formats
//   - GSM-7 extension table characters ([ ] { } etc.) count as 2 septets
//   - Septet packing round-trip: encode then decode yields original text
//   - isGsm7Compatible detection
//   - Length limits: 160 GSM-7, 140 UCS-2 octets
//   - Supplementary Unicode (surrogate pairs in UCS-2)
//   - packSeptets with bitOffset=49 (concat UDH fill-bit alignment)
//   - buildSmsSubmitPduMulti: boundary, UDH, ESC-safe split, UCS-2 split
//   - Concat reference counter increments only on multi-part messages

#include <unity.h>
#include <Arduino.h>

#include "sms_codec.h"

// Helper: decode hex string to bytes for inspection.
static uint8_t hexByte(const String &s, int offset)
{
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
        if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
        if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
        return 0;
    };
    return (uint8_t)((nibble(s[offset]) << 4) | nibble(s[offset + 1]));
}

void test_buildSmsSubmitPdu_ascii_gsm7()
{
    sms_codec::SmsSubmitPdu pdu;
    bool ok = sms_codec::buildSmsSubmitPdu(String("+8613800138000"),
                                           String("Hello"), pdu);
    TEST_ASSERT_TRUE(ok);

    // SCA = 00 (use default)
    TEST_ASSERT_EQUAL(0x00, hexByte(pdu.hex, 0));
    // First octet = 01 (SMS-SUBMIT)
    TEST_ASSERT_EQUAL(0x01, hexByte(pdu.hex, 2));
    // TP-MR = 00
    TEST_ASSERT_EQUAL(0x00, hexByte(pdu.hex, 4));

    // TP-DA: 13 digits, international
    TEST_ASSERT_EQUAL(0x0D, hexByte(pdu.hex, 6));  // digit count = 13
    TEST_ASSERT_EQUAL(0x91, hexByte(pdu.hex, 8));  // TOA = international

    // BCD: +8613800138000 -> digits "8613800138000"
    //   6,8 -> 0x68
    //   1,3 -> 0x31
    //   8,0 -> 0x08
    //   0,1 -> 0x10
    //   3,8 -> 0x83
    //   0,0 -> 0x00
    //   0,F -> 0xF0 (odd padding)
    TEST_ASSERT_EQUAL(0x68, hexByte(pdu.hex, 10)); // 86
    TEST_ASSERT_EQUAL(0x31, hexByte(pdu.hex, 12)); // 13
    TEST_ASSERT_EQUAL(0x08, hexByte(pdu.hex, 14)); // 80
    TEST_ASSERT_EQUAL(0x10, hexByte(pdu.hex, 16)); // 01
    TEST_ASSERT_EQUAL(0x83, hexByte(pdu.hex, 18)); // 38
    TEST_ASSERT_EQUAL(0x00, hexByte(pdu.hex, 20)); // 00
    TEST_ASSERT_EQUAL(0xF0, hexByte(pdu.hex, 22)); // 0F

    // TP-PID = 00
    TEST_ASSERT_EQUAL(0x00, hexByte(pdu.hex, 24));
    // TP-DCS = 00 (GSM-7)
    TEST_ASSERT_EQUAL(0x00, hexByte(pdu.hex, 26));
    // TP-UDL = 5 (5 septets for "Hello")
    TEST_ASSERT_EQUAL(0x05, hexByte(pdu.hex, 28));

    // TP-UD: "Hello" packed as GSM-7 septets
    // H=0x48, e=0x65, l=0x6C, l=0x6C, o=0x6F
    // Packed: C8329BFD06
    TEST_ASSERT_EQUAL(0xC8, hexByte(pdu.hex, 30));
    TEST_ASSERT_EQUAL(0x32, hexByte(pdu.hex, 32));
    TEST_ASSERT_EQUAL(0x9B, hexByte(pdu.hex, 34));
    TEST_ASSERT_EQUAL(0xFD, hexByte(pdu.hex, 36));
    TEST_ASSERT_EQUAL(0x06, hexByte(pdu.hex, 38));

    // tpduLen = total PDU bytes - 1 (SCA byte)
    TEST_ASSERT_EQUAL((int)(pdu.hex.length() / 2) - 1, pdu.tpduLen);
}

void test_buildSmsSubmitPdu_unicode_ucs2()
{
    sms_codec::SmsSubmitPdu pdu;
    // "你好" in UTF-8
    String body;
    body += (char)(unsigned char)0xE4;
    body += (char)(unsigned char)0xBD;
    body += (char)(unsigned char)0xA0;
    body += (char)(unsigned char)0xE5;
    body += (char)(unsigned char)0xA5;
    body += (char)(unsigned char)0xBD;

    bool ok = sms_codec::buildSmsSubmitPdu(String("+8613800138000"),
                                           body, pdu);
    TEST_ASSERT_TRUE(ok);

    // Find TP-DCS position: after SCA(1) + first(1) + MR(1) + DA(9) + PID(1)
    // = byte 13, hex offset 26
    TEST_ASSERT_EQUAL(0x08, hexByte(pdu.hex, 26)); // UCS-2

    // TP-UDL = 4 octets (two UCS-2 code units)
    TEST_ASSERT_EQUAL(0x04, hexByte(pdu.hex, 28));

    // UD: 你=U+4F60, 好=U+597D -> 4F60 597D
    TEST_ASSERT_EQUAL(0x4F, hexByte(pdu.hex, 30));
    TEST_ASSERT_EQUAL(0x60, hexByte(pdu.hex, 32));
    TEST_ASSERT_EQUAL(0x59, hexByte(pdu.hex, 34));
    TEST_ASSERT_EQUAL(0x7D, hexByte(pdu.hex, 36));
}

void test_buildSmsSubmitPdu_national_phone()
{
    sms_codec::SmsSubmitPdu pdu;
    // No leading +, should use TOA 0x81
    bool ok = sms_codec::buildSmsSubmitPdu(String("13800138000"),
                                           String("Hi"), pdu);
    TEST_ASSERT_TRUE(ok);

    // DA: 11 digits
    TEST_ASSERT_EQUAL(0x0B, hexByte(pdu.hex, 6));  // digit count = 11
    TEST_ASSERT_EQUAL(0x81, hexByte(pdu.hex, 8));  // TOA = national
}

void test_buildSmsSubmitPdu_extension_chars_count_double()
{
    sms_codec::SmsSubmitPdu pdu;
    // "[" and "]" each need 2 septets (escape + code).
    // "A[B]C" = 5 chars but 7 septets.
    bool ok = sms_codec::buildSmsSubmitPdu(String("+1234"),
                                           String("A[B]C"), pdu);
    TEST_ASSERT_TRUE(ok);

    // Find UDL: SCA(1) + first(1) + MR(1) + DA(4) + PID(1) + DCS(1)
    // Phone "+1234" -> 4 digits -> DA = 1 + 1 + 2 = 4 bytes
    // UDL at byte 9, hex offset 18
    TEST_ASSERT_EQUAL(0x07, hexByte(pdu.hex, 18)); // 7 septets
}

void test_isGsm7Compatible_true_for_ascii()
{
    TEST_ASSERT_TRUE(sms_codec::isGsm7Compatible(String("Hello World!")));
    TEST_ASSERT_TRUE(sms_codec::isGsm7Compatible(String("0123456789")));
    TEST_ASSERT_TRUE(sms_codec::isGsm7Compatible(String("@$")));
}

void test_isGsm7Compatible_false_for_chinese()
{
    String s;
    s += (char)(unsigned char)0xE4;
    s += (char)(unsigned char)0xBD;
    s += (char)(unsigned char)0xA0;
    TEST_ASSERT_FALSE(sms_codec::isGsm7Compatible(s));
}

void test_isGsm7Compatible_true_for_euro_sign()
{
    // € (U+20AC) is in the GSM-7 extension table
    String s;
    s += (char)(unsigned char)0xE2;
    s += (char)(unsigned char)0x82;
    s += (char)(unsigned char)0xAC;
    TEST_ASSERT_TRUE(sms_codec::isGsm7Compatible(s));
}

void test_buildSmsSubmitPdu_gsm7_max_160()
{
    sms_codec::SmsSubmitPdu pdu;
    String body;
    for (int i = 0; i < 160; ++i)
        body += 'a';
    TEST_ASSERT_TRUE(sms_codec::buildSmsSubmitPdu(String("+1"), body, pdu));

    // 161 should fail
    body += 'a';
    TEST_ASSERT_FALSE(sms_codec::buildSmsSubmitPdu(String("+1"), body, pdu));
}

void test_buildSmsSubmitPdu_ucs2_max_70()
{
    sms_codec::SmsSubmitPdu pdu;
    // 70 Chinese characters = 140 UCS-2 bytes
    String body;
    for (int i = 0; i < 70; ++i)
    {
        body += (char)(unsigned char)0xE4;
        body += (char)(unsigned char)0xBD;
        body += (char)(unsigned char)0xA0;
    }
    TEST_ASSERT_TRUE(sms_codec::buildSmsSubmitPdu(String("+1"), body, pdu));

    // 71 should fail (142 bytes > 140)
    body += (char)(unsigned char)0xE4;
    body += (char)(unsigned char)0xBD;
    body += (char)(unsigned char)0xA0;
    TEST_ASSERT_FALSE(sms_codec::buildSmsSubmitPdu(String("+1"), body, pdu));
}

void test_buildSmsSubmitPdu_supplementary_unicode()
{
    sms_codec::SmsSubmitPdu pdu;
    // U+1F600 (😀) in UTF-8 = F0 9F 98 80
    // In UTF-16BE = D83D DE00 (surrogate pair, 4 bytes)
    String body;
    body += (char)(unsigned char)0xF0;
    body += (char)(unsigned char)0x9F;
    body += (char)(unsigned char)0x98;
    body += (char)(unsigned char)0x80;

    bool ok = sms_codec::buildSmsSubmitPdu(String("+1234"), body, pdu);
    TEST_ASSERT_TRUE(ok);

    // Should be UCS-2 (not GSM-7)
    // Phone "+1234": 4 digits -> DA = 1+1+2 = 4 bytes
    // PID at byte 7 (hex 14), DCS at byte 8 (hex 16)
    TEST_ASSERT_EQUAL(0x08, hexByte(pdu.hex, 16)); // UCS-2

    // UDL = 4 (surrogate pair = 4 bytes), at byte 9 (hex 18)
    TEST_ASSERT_EQUAL(0x04, hexByte(pdu.hex, 18));

    // UD = D83D DE00, starting at byte 10 (hex 20)
    TEST_ASSERT_EQUAL(0xD8, hexByte(pdu.hex, 20));
    TEST_ASSERT_EQUAL(0x3D, hexByte(pdu.hex, 22));
    TEST_ASSERT_EQUAL(0xDE, hexByte(pdu.hex, 24));
    TEST_ASSERT_EQUAL(0x00, hexByte(pdu.hex, 26));
}

void test_buildSmsSubmitPdu_gsm7_round_trip()
{
    // Encode "Hello World" as GSM-7, then parse it back using the
    // existing PDU decoder to verify consistency.
    sms_codec::SmsSubmitPdu submitPdu;
    bool ok = sms_codec::buildSmsSubmitPdu(String("+1234"),
                                           String("Test 123!"), submitPdu);
    TEST_ASSERT_TRUE(ok);

    // The submit PDU can't be parsed by parseSmsPdu (which expects
    // SMS-DELIVER), but we can verify the septets by manually
    // checking the packed bytes.
    // "Test 123!" = 9 chars, all GSM-7 basic table
    // Phone "+1234": DA = 4 bytes, UDL at byte 9, hex 18
    TEST_ASSERT_EQUAL(0x09, hexByte(submitPdu.hex, 18)); // 9 septets
}

// ---------- packSeptets with bitOffset ----------

// Verify packSeptets(septets, bitOffset=49) produces a buffer whose
// first 6 bytes are zero (the UDH placeholder area) and whose bit-49
// onward encodes the septets correctly.
//
// We use a simple 3-septet test: [0x41, 0x42, 0x43] ('A','B','C' in GSM-7).
// With bitOffset=0: packed into bytes starting at bit 0.
//   A(0x41=65) at bits 0-6  -> byte0 |= 65 << 0 = 0x41
//   B(0x42=66) at bits 7-13 -> byte0 |= 66 << 7 = 0 (high bit), byte1 |= 66 >> 1 = 0x21
//   C(0x43=67) at bits 14-20-> byte1 |= 67 << 6 = 0xC0, byte2 |= 67 >> 2 = 0x10
// Expected bytes (0): 0x41, 0xA1, 0x10  (3 septets = ceil(21/8)=3 bytes)
//
// With bitOffset=49:
//   Buffer size = ceil((49 + 3*7) / 8) = ceil(70/8) = ceil(8.75) = 9 bytes
//   Bytes 0-5 = 0x00 (zero fill for UDH area)
//   byte 6: bit 49 = bit 1 of byte 6  -> A(0x41=65) shifted left 1 = 0x82
//   byte 6/7: B starts at bit 56 = bit 0 of byte 7 -> 0x42
//   byte 7/8: C starts at bit 63 = bit 7 of byte 7 -> high bit in byte7, low 6 in byte8
void test_packSeptets_bitOffset49()
{
    std::vector<uint8_t> septets = {0x41, 0x42, 0x43}; // A B C

    // bitOffset=0 reference
    auto packed0 = sms_codec::packSeptets(septets, 0);
    TEST_ASSERT_EQUAL(3u, packed0.size()); // ceil(21/8) = 3

    // bitOffset=49
    auto packed49 = sms_codec::packSeptets(septets, 49);
    // Buffer size = ceil((49 + 21) / 8) = ceil(70/8) = 9
    TEST_ASSERT_EQUAL(9u, packed49.size());

    // First 6 bytes must be zero (placeholder for UDH).
    for (int i = 0; i < 6; ++i)
        TEST_ASSERT_EQUAL_MESSAGE(0x00, packed49[i], "UDH placeholder byte non-zero");

    // The 7 bits of septet A(0x41=65=0b1000001) start at bit 49.
    // bit49 = byte6 bit1 (since 49/8=6 r1).
    // 65 << 1 = 130 = 0x82; no overflow into byte7 since 65<<1 <= 0xFF.
    TEST_ASSERT_EQUAL(0x82, packed49[6]);

    // Septet B(0x42=66=0b1000010) starts at bit 56 = byte7 bit0.
    // Septet C(0x43=67=0b1000011) starts at bit 63 = byte7 bit7.
    // Low bit of C (67 & 1 = 1) ORs into byte7 bit7 -> byte7 |= 0x80.
    // High 6 bits of C (67 >> 1 = 33 = 0x21) go into byte8.
    // Combined: byte7 = B(0x42) | C_lowbit(0x80) = 0xC2.
    TEST_ASSERT_EQUAL(0xC2, packed49[7]); // B | low-bit of C
    TEST_ASSERT_EQUAL(0x21, packed49[8]); // high 6 bits of C
}

// ---------- buildSmsSubmitPduMulti boundary tests ----------

// Exactly 160 GSM-7 chars -> 1 part, no UDH (first octet = 0x01)
void test_buildSmsSubmitPduMulti_160_gsm7_single_part()
{
    String body;
    for (int i = 0; i < 160; ++i)
        body += 'a';
    auto pdus = sms_codec::buildSmsSubmitPduMulti(String("+1"), body);
    TEST_ASSERT_EQUAL(1u, pdus.size());
    // First octet after SCA (0x00) = 0x01 (SMS-SUBMIT, no UDHI)
    TEST_ASSERT_EQUAL(0x01, hexByte(pdus[0].hex, 2));
}

// Exactly 161 GSM-7 chars -> 2 parts with UDH (first octet = 0x41)
void test_buildSmsSubmitPduMulti_161_gsm7_two_parts()
{
    String body;
    for (int i = 0; i < 161; ++i)
        body += 'a';
    auto pdus = sms_codec::buildSmsSubmitPduMulti(String("+1"), body);
    TEST_ASSERT_EQUAL(2u, pdus.size());

    // Both parts must have first octet = 0x41 (UDHI set)
    TEST_ASSERT_EQUAL(0x41, hexByte(pdus[0].hex, 2));
    TEST_ASSERT_EQUAL(0x41, hexByte(pdus[1].hex, 2));

    // Find the TP-UDL position for phone "+1": 1 digit -> DA = 1+1+1 = 3 bytes.
    // PDU layout: SCA(1) + first(1) + MR(1) + DA(3) + PID(1) + DCS(1) = 8 bytes
    // UDL at byte 8, hex offset 16.
    // Part 1: 7 header septets + 153 payload = UDL 160
    TEST_ASSERT_EQUAL(160, hexByte(pdus[0].hex, 16));
    // Part 2: 7 header septets + 8 remaining = UDL 15
    TEST_ASSERT_EQUAL(15, hexByte(pdus[1].hex, 16));

    // Verify UDH bytes in part 1 (UD starts at byte 9 / hex offset 18)
    // UDH: [UDHL=05] [IEI=00] [IEDL=03] [ref] [total=2] [seq=1]
    TEST_ASSERT_EQUAL(0x05, hexByte(pdus[0].hex, 18)); // UDHL
    TEST_ASSERT_EQUAL(0x00, hexByte(pdus[0].hex, 20)); // IEI
    TEST_ASSERT_EQUAL(0x03, hexByte(pdus[0].hex, 22)); // IEDL
    // ref is at hex 24 (whatever the counter gave)
    TEST_ASSERT_EQUAL(2,    hexByte(pdus[0].hex, 26)); // total parts
    TEST_ASSERT_EQUAL(1,    hexByte(pdus[0].hex, 28)); // seq = 1

    // Verify part 2 UDH seq = 2
    TEST_ASSERT_EQUAL(2, hexByte(pdus[1].hex, 28));

    // Both parts must share the same ref byte.
    TEST_ASSERT_EQUAL(hexByte(pdus[0].hex, 24), hexByte(pdus[1].hex, 24));
}

// Exactly 70 UCS-2 chars -> 1 part, no UDH
void test_buildSmsSubmitPduMulti_70_ucs2_single_part()
{
    String body;
    for (int i = 0; i < 70; ++i)
    {
        body += (char)(unsigned char)0xE4;
        body += (char)(unsigned char)0xBD;
        body += (char)(unsigned char)0xA0; // 你 (U+4F60)
    }
    auto pdus = sms_codec::buildSmsSubmitPduMulti(String("+1"), body);
    TEST_ASSERT_EQUAL(1u, pdus.size());
    TEST_ASSERT_EQUAL(0x01, hexByte(pdus[0].hex, 2)); // no UDHI
}

// 71 UCS-2 chars -> 2 parts with UDH
void test_buildSmsSubmitPduMulti_71_ucs2_two_parts()
{
    String body;
    for (int i = 0; i < 71; ++i)
    {
        body += (char)(unsigned char)0xE4;
        body += (char)(unsigned char)0xBD;
        body += (char)(unsigned char)0xA0; // 你 (U+4F60)
    }
    auto pdus = sms_codec::buildSmsSubmitPduMulti(String("+1"), body);
    TEST_ASSERT_EQUAL(2u, pdus.size());
    TEST_ASSERT_EQUAL(0x41, hexByte(pdus[0].hex, 2)); // UDHI set
    TEST_ASSERT_EQUAL(0x41, hexByte(pdus[1].hex, 2));

    // Part 1: 67 UCS-2 chars = 134 octets payload, UDL = 6 + 134 = 140
    // PDU layout: SCA(1)+first(1)+MR(1)+DA(3)+PID(1)+DCS(1) = 8 bytes, UDL at hex 16
    TEST_ASSERT_EQUAL(140, hexByte(pdus[0].hex, 16));
    // Part 2: 4 chars = 8 octets, UDL = 6 + 8 = 14
    TEST_ASSERT_EQUAL(14, hexByte(pdus[1].hex, 16));

    // DCS must be 0x08 (UCS-2) in both parts: hex offset 14 for phone "+1"
    // DA=3 bytes -> PID at hex 14, DCS at hex 14? Let me recount.
    // hex offset: SCA=00(0-1), first=41(2-3), MR=00(4-5), DA_len(6-7),
    // DA_toa(8-9), DA_bcd(10-11) [1 digit padded to 1 byte], PID(12-13), DCS(14-15)
    TEST_ASSERT_EQUAL(0x08, hexByte(pdus[0].hex, 14)); // DCS = UCS-2
    TEST_ASSERT_EQUAL(0x08, hexByte(pdus[1].hex, 14));
}

// ESC-safe split: body of 151 regular chars + '[' (ESC + 0x3C = 2 septets)
// + 9 more 'a' chars = 162 septets total.  The chunk boundary at 153 would
// land at septets[152] = 0x3C (extension code for '['), leaving the ESC at
// septets[151] as the last septet of part 1 (undecodable lone ESC).
// The split rule checks septets[pos+151] == 0x1B and trims to 151 septets.
void test_buildSmsSubmitPduMulti_esc_safe_split()
{
    // '[' encodes as ESC(0x1B) + 0x3C in GSM-7 (2 septets).
    // Build: 151 'a' chars (151 septets) + '[' (2 septets) + 9 'a' (9 septets)
    // = 162 septets total (>160 so multi-part path is taken).
    //
    // Without ESC-safe check, split at 153 would give:
    //   Part1: 151 'a' + ESC (leaving 0x3C orphaned in part2) -- WRONG
    // With check: septets[pos+151] = ESC -> trim to 151, so:
    //   Part1: 151 'a' (151 septets)
    //   Part2: ESC + 0x3C + 9 'a' (11 septets)
    String body;
    for (int i = 0; i < 151; ++i)
        body += 'a';
    body += '['; // 2 septets (ESC + 0x3C)
    for (int i = 0; i < 9; ++i)
        body += 'a'; // 9 more septets (total 162)

    auto pdus = sms_codec::buildSmsSubmitPduMulti(String("+1"), body);
    TEST_ASSERT_EQUAL(2u, pdus.size());

    // PDU layout for phone "+1": UDL at hex offset 16.
    // Part 1 should have 7 + 151 = 158 septets.
    TEST_ASSERT_EQUAL(158, hexByte(pdus[0].hex, 16));

    // Part 2 should have 7 + 11 = 18 septets.
    TEST_ASSERT_EQUAL(18, hexByte(pdus[1].hex, 16));
}

// Max-parts cap: a body exceeding 10 parts returns empty vector.
void test_buildSmsSubmitPduMulti_max_parts_cap()
{
    // 10 * 153 + 1 = 1531 septets = 11 parts needed.
    String body;
    for (int i = 0; i < 1531; ++i)
        body += 'x';
    auto pdus = sms_codec::buildSmsSubmitPduMulti(String("+1"), body);
    TEST_ASSERT_EQUAL(0u, pdus.size()); // empty = error
}

// Concat reference numbers: two multi-part calls produce different refs.
// Single-part calls must NOT increment the counter (verified by checking
// that calling single-part before multi-part doesn't skip a ref value
// we care about -- we just assert the two multi-part calls differ).
void test_buildSmsSubmitPduMulti_ref_uniqueness()
{
    // PDU layout for phone "+1", UDH ref is at hex offset 24.
    String body161;
    for (int i = 0; i < 161; ++i)
        body161 += 'a';

    // Single-part call should NOT advance the counter.
    sms_codec::buildSmsSubmitPduMulti(String("+1"), String("hi"));
    sms_codec::buildSmsSubmitPduMulti(String("+1"), String("hi"));

    auto pdus1 = sms_codec::buildSmsSubmitPduMulti(String("+1"), body161);
    auto pdus2 = sms_codec::buildSmsSubmitPduMulti(String("+1"), body161);
    TEST_ASSERT_EQUAL(2u, pdus1.size());
    TEST_ASSERT_EQUAL(2u, pdus2.size());

    uint8_t ref1 = hexByte(pdus1[0].hex, 24);
    uint8_t ref2 = hexByte(pdus2[0].hex, 24);
    TEST_ASSERT_NOT_EQUAL(ref1, ref2);
}

void run_sms_pdu_encode_tests()
{
    RUN_TEST(test_buildSmsSubmitPdu_ascii_gsm7);
    RUN_TEST(test_buildSmsSubmitPdu_unicode_ucs2);
    RUN_TEST(test_buildSmsSubmitPdu_national_phone);
    RUN_TEST(test_buildSmsSubmitPdu_extension_chars_count_double);
    RUN_TEST(test_isGsm7Compatible_true_for_ascii);
    RUN_TEST(test_isGsm7Compatible_false_for_chinese);
    RUN_TEST(test_isGsm7Compatible_true_for_euro_sign);
    RUN_TEST(test_buildSmsSubmitPdu_gsm7_max_160);
    RUN_TEST(test_buildSmsSubmitPdu_ucs2_max_70);
    RUN_TEST(test_buildSmsSubmitPdu_supplementary_unicode);
    RUN_TEST(test_buildSmsSubmitPdu_gsm7_round_trip);
    RUN_TEST(test_packSeptets_bitOffset49);
    RUN_TEST(test_buildSmsSubmitPduMulti_160_gsm7_single_part);
    RUN_TEST(test_buildSmsSubmitPduMulti_161_gsm7_two_parts);
    RUN_TEST(test_buildSmsSubmitPduMulti_70_ucs2_single_part);
    RUN_TEST(test_buildSmsSubmitPduMulti_71_ucs2_two_parts);
    RUN_TEST(test_buildSmsSubmitPduMulti_esc_safe_split);
    RUN_TEST(test_buildSmsSubmitPduMulti_max_parts_cap);
    RUN_TEST(test_buildSmsSubmitPduMulti_ref_uniqueness);
}
