// Tests for sms_codec::parseSmsPdu (RFC-0002). Feeds handcrafted
// SMS-DELIVER PDUs through the parser and verifies sender, content,
// timestamp, and concatenation metadata round-trip correctly. Uses
// the shared PDU builder in pdu_test_helpers.h so the same fixtures
// are reused by the sms_handler PDU tests.

#include <unity.h>
#include <Arduino.h>

#include "sms_codec.h"
#include "pdu_test_helpers.h"

using pdu_test::buildPduHex;
using pdu_test::gsm7FromAscii;
using pdu_test::PduBuildOpts;
using pdu_test::utf8ToUtf16Be;
using sms_codec::parseSmsPdu;
using sms_codec::SmsPdu;

void test_parseSmsPdu_single_part_gsm7()
{
    PduBuildOpts opts;
    opts.sender = "13800138000";
    opts.dcs = 0x00;
    opts.bodyBytes = gsm7FromAscii("Hello");
    String hex = buildPduHex(opts);

    SmsPdu pdu;
    TEST_ASSERT_TRUE(parseSmsPdu(hex, pdu));
    TEST_ASSERT_EQUAL_STRING("+13800138000", pdu.sender.c_str());
    TEST_ASSERT_EQUAL_STRING("Hello", pdu.content.c_str());
    TEST_ASSERT_EQUAL_STRING("24/01/15,10:30:45+32", pdu.timestamp.c_str());
    TEST_ASSERT_FALSE(pdu.isConcatenated);
}

void test_parseSmsPdu_single_part_ucs2_chinese()
{
    PduBuildOpts opts;
    opts.sender = "10086";
    opts.dcs = 0x08;
    opts.bodyBytes = utf8ToUtf16Be("\xE4\xB8\xAD\xE6\x96\x87"); // 中文
    String hex = buildPduHex(opts);

    SmsPdu pdu;
    TEST_ASSERT_TRUE(parseSmsPdu(hex, pdu));
    TEST_ASSERT_EQUAL_STRING("+10086", pdu.sender.c_str());
    TEST_ASSERT_EQUAL_STRING("\xE4\xB8\xAD\xE6\x96\x87", pdu.content.c_str());
    TEST_ASSERT_FALSE(pdu.isConcatenated);
}

void test_parseSmsPdu_concat_gsm7_2part_metadata()
{
    PduBuildOpts p1;
    p1.sender = "13800138000";
    p1.dcs = 0x00;
    p1.bodyBytes = gsm7FromAscii("Hello ");
    p1.addConcatUdh = true;
    p1.concatRef = 0x42;
    p1.concatTotal = 2;
    p1.concatPart = 1;
    String hex1 = buildPduHex(p1);

    PduBuildOpts p2 = p1;
    p2.bodyBytes = gsm7FromAscii("World");
    p2.concatPart = 2;
    String hex2 = buildPduHex(p2);

    SmsPdu pdu1, pdu2;
    TEST_ASSERT_TRUE(parseSmsPdu(hex1, pdu1));
    TEST_ASSERT_TRUE(parseSmsPdu(hex2, pdu2));

    TEST_ASSERT_TRUE(pdu1.isConcatenated);
    TEST_ASSERT_EQUAL(0x42, pdu1.concatRefNumber);
    TEST_ASSERT_EQUAL(2, pdu1.concatTotalParts);
    TEST_ASSERT_EQUAL(1, pdu1.concatPartNumber);
    TEST_ASSERT_EQUAL_STRING("Hello ", pdu1.content.c_str());

    TEST_ASSERT_TRUE(pdu2.isConcatenated);
    TEST_ASSERT_EQUAL(2, pdu2.concatPartNumber);
    TEST_ASSERT_EQUAL_STRING("World", pdu2.content.c_str());
}

void test_parseSmsPdu_concat_ucs2_16bit_ref_metadata()
{
    PduBuildOpts o;
    o.sender = "12025550170";
    o.dcs = 0x08;
    o.addConcatUdh = true;
    o.udh16bit = true;
    o.concatRef = 0x1234;
    o.concatTotal = 3;
    o.concatPart = 2;
    o.bodyBytes = utf8ToUtf16Be("\xE8\xAA\x9E\xE3\x81\xA7"); // 語で
    String hex = buildPduHex(o);

    SmsPdu pdu;
    TEST_ASSERT_TRUE(parseSmsPdu(hex, pdu));
    TEST_ASSERT_TRUE(pdu.isConcatenated);
    TEST_ASSERT_EQUAL(0x1234, pdu.concatRefNumber);
    TEST_ASSERT_EQUAL(3, pdu.concatTotalParts);
    TEST_ASSERT_EQUAL(2, pdu.concatPartNumber);
    TEST_ASSERT_EQUAL_STRING("\xE8\xAA\x9E\xE3\x81\xA7", pdu.content.c_str());
}

void test_parseSmsPdu_surrogate_pair_ucs2()
{
    // U+1F600 GRINNING FACE.
    PduBuildOpts opts;
    opts.sender = "10086";
    opts.dcs = 0x08;
    opts.bodyBytes = {0xD8, 0x3D, 0xDE, 0x00};
    String hex = buildPduHex(opts);

    SmsPdu pdu;
    TEST_ASSERT_TRUE(parseSmsPdu(hex, pdu));
    TEST_ASSERT_EQUAL_STRING("\xF0\x9F\x98\x80", pdu.content.c_str());
}

void test_parseSmsPdu_malformed_short_pdu()
{
    SmsPdu pdu;
    TEST_ASSERT_FALSE(parseSmsPdu(String("00040B91"), pdu));
}

void test_parseSmsPdu_invalid_hex_returns_false()
{
    SmsPdu pdu;
    TEST_ASSERT_FALSE(parseSmsPdu(String("NOTHEX"), pdu));
}

void test_parseSmsPdu_gsm7_with_udh_septet_padding()
{
    // 6-byte UDH (8-bit ref) forces exactly 1 fill bit of septet
    // padding. The first body septet starts at bit 49 of the UD.
    // Exercise that packing/unpacking is symmetric across fill bits.
    PduBuildOpts opts;
    opts.sender = "13800138000";
    opts.dcs = 0x00;
    opts.bodyBytes = gsm7FromAscii("ABCDEFG"); // 7 septets
    opts.addConcatUdh = true;
    opts.concatRef = 1;
    opts.concatTotal = 2;
    opts.concatPart = 1;
    String hex = buildPduHex(opts);

    SmsPdu pdu;
    TEST_ASSERT_TRUE(parseSmsPdu(hex, pdu));
    TEST_ASSERT_EQUAL_STRING("ABCDEFG", pdu.content.c_str());
    TEST_ASSERT_TRUE(pdu.isConcatenated);
    TEST_ASSERT_EQUAL(1, pdu.concatPartNumber);
}

void run_sms_pdu_tests()
{
    RUN_TEST(test_parseSmsPdu_single_part_gsm7);
    RUN_TEST(test_parseSmsPdu_single_part_ucs2_chinese);
    RUN_TEST(test_parseSmsPdu_concat_gsm7_2part_metadata);
    RUN_TEST(test_parseSmsPdu_concat_ucs2_16bit_ref_metadata);
    RUN_TEST(test_parseSmsPdu_surrogate_pair_ucs2);
    RUN_TEST(test_parseSmsPdu_malformed_short_pdu);
    RUN_TEST(test_parseSmsPdu_invalid_hex_returns_false);
    RUN_TEST(test_parseSmsPdu_gsm7_with_udh_septet_padding);
}
