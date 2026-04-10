// Unit tests for src/sms_codec.{h,cpp}. Covers the pure helpers:
// decodeUCS2, parseCmgrBody, humanReadablePhoneNumber,
// timestampToRFC3339. Runs on the host via [env:native] in
// platformio.ini.

#include <unity.h>
#include <Arduino.h>

#include "sms_codec.h"

using namespace sms_codec;

// ---------- decodeUCS2 ----------

void test_decodeUCS2_plain_ascii_passthrough()
{
    // Non-hex input (contains 'Z' after whitespace stripping) should be
    // returned unchanged aside from the whitespace strip the decoder
    // always performs on its input.
    String out = decodeUCS2(String("HelloZorld"));
    TEST_ASSERT_EQUAL_STRING("HelloZorld", out.c_str());
}

void test_decodeUCS2_ascii_bytes_in_hex()
{
    // Even-length hex but not a multiple of 4 -> treated as ASCII bytes.
    // "Hi" = 48 69 in hex = 4 chars, which IS a multiple of 4 and would
    // actually decode as UTF-16BE 0x4869 = Chinese character. Use 6-char
    // hex "414243" = "ABC".
    String out = decodeUCS2(String("414243"));
    TEST_ASSERT_EQUAL_STRING("ABC", out.c_str());
}

void test_decodeUCS2_chinese_bmp()
{
    // UCS2 for 中文: 4E2D 6587 -> UTF-8 E4 B8 AD E6 96 87
    String out = decodeUCS2(String("4E2D6587"));
    const unsigned char expected[] = {0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87, 0x00};
    TEST_ASSERT_EQUAL_STRING((const char *)expected, out.c_str());
}

void test_decodeUCS2_surrogate_pair()
{
    // U+1F600 GRINNING FACE:
    //   high surrogate D83D, low surrogate DE00
    // UTF-8: F0 9F 98 80
    String out = decodeUCS2(String("D83DDE00"));
    const unsigned char expected[] = {0xF0, 0x9F, 0x98, 0x80, 0x00};
    TEST_ASSERT_EQUAL_STRING((const char *)expected, out.c_str());
}

void test_decodeUCS2_odd_length_passthrough()
{
    // Odd length -> not valid hex -> returned unchanged.
    String out = decodeUCS2(String("ABCDE"));
    TEST_ASSERT_EQUAL_STRING("ABCDE", out.c_str());
}

void test_decodeUCS2_non_hex_char_passthrough()
{
    // Contains 'G' -> not hex -> returned unchanged.
    String out = decodeUCS2(String("41G2"));
    TEST_ASSERT_EQUAL_STRING("41G2", out.c_str());
}

void test_decodeUCS2_empty_input()
{
    String out = decodeUCS2(String(""));
    TEST_ASSERT_EQUAL_STRING("", out.c_str());
}

void test_decodeUCS2_mixed_whitespace()
{
    // Whitespace should be stripped before hex parsing.
    String out = decodeUCS2(String(" 4E2D\r\n6587 \t"));
    const unsigned char expected[] = {0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87, 0x00};
    TEST_ASSERT_EQUAL_STRING((const char *)expected, out.c_str());
}

// ---------- parseCmgrBody ----------

void test_parseCmgrBody_happy_path()
{
    // Well-formed response: sender = UCS2 for +8613800138000 (ASCII in UCS2),
    // body = UCS2 for "Hello".
    // +8613800138000 in UCS2: 002B 0038 0036 0031 0033 0038 0030 0030 0031 0033 0038 0030 0030 0030
    // "Hello" in UCS2: 0048 0065 006C 006C 006F
    String raw = String(
        "+CMGR: \"REC UNREAD\","
        "\"00310033003800300030003100330038003000300030\","
        "\"\",\"24/01/15,10:30:45+32\"\r\n"
        "00480065006C006C006F\r\n"
        "\r\nOK\r\n");

    String sender, ts, content;
    TEST_ASSERT_TRUE(parseCmgrBody(raw, sender, ts, content));
    TEST_ASSERT_EQUAL_STRING("13800138000", sender.c_str());
    TEST_ASSERT_EQUAL_STRING("24/01/15,10:30:45+32", ts.c_str());
    TEST_ASSERT_EQUAL_STRING("Hello", content.c_str());
}

void test_parseCmgrBody_missing_trailing_ok()
{
    // No "\r\nOK" marker. Parser should still return true (bodyEnd
    // falls through to raw.length()) — matches original behaviour.
    String raw = String(
        "+CMGR: \"REC UNREAD\","
        "\"00310033003800300030003100330038003000300030\","
        "\"\",\"24/01/15,10:30:45+32\"\r\n"
        "00480065006C006C006F");

    String sender, ts, content;
    TEST_ASSERT_TRUE(parseCmgrBody(raw, sender, ts, content));
    TEST_ASSERT_EQUAL_STRING("13800138000", sender.c_str());
    TEST_ASSERT_EQUAL_STRING("Hello", content.c_str());
}

void test_parseCmgrBody_missing_eighth_quote()
{
    // Header has only 7 quotes -> parse must return false.
    String raw = String(
        "+CMGR: \"REC UNREAD\","
        "\"00310033003800300030003100330038003000300030\","
        "\"\",\"24/01/15,10:30:45+32\r\n"
        "00480065006C006C006F\r\n"
        "\r\nOK\r\n");

    String sender, ts, content;
    TEST_ASSERT_FALSE(parseCmgrBody(raw, sender, ts, content));
}

void test_parseCmgrBody_empty_body()
{
    // Well-formed header, no content.
    String raw = String(
        "+CMGR: \"REC UNREAD\","
        "\"00310033003800300030003100330038003000300030\","
        "\"\",\"24/01/15,10:30:45+32\"\r\n"
        "\r\nOK\r\n");

    String sender, ts, content;
    TEST_ASSERT_TRUE(parseCmgrBody(raw, sender, ts, content));
    TEST_ASSERT_EQUAL_STRING("13800138000", sender.c_str());
    TEST_ASSERT_EQUAL_STRING("", content.c_str());
}

void test_parseCmgrBody_no_cmgr_header()
{
    String raw = String("OK\r\n");
    String sender, ts, content;
    TEST_ASSERT_FALSE(parseCmgrBody(raw, sender, ts, content));
}

// ---------- humanReadablePhoneNumber ----------

void test_humanReadablePhoneNumber_11_digit()
{
    TEST_ASSERT_EQUAL_STRING("+86 138-0013-8000",
                             humanReadablePhoneNumber(String("13800138000")).c_str());
}

void test_humanReadablePhoneNumber_86_prefix_13_char()
{
    // The +86 branch fires at length == 13, which matches "+86" + 10
    // digits — NOT a real Chinese mobile (11 digits). We're asserting
    // current behaviour, not the ideal one; see the comment in
    // sms_codec::humanReadablePhoneNumber.
    TEST_ASSERT_EQUAL_STRING("+86 138-0013-800",
                             humanReadablePhoneNumber(String("+861380013800")).c_str());
}

void test_humanReadablePhoneNumber_86_prefix_14_char_passthrough()
{
    // "+86" + 11 digits = 14 chars, matches neither branch -> unchanged.
    TEST_ASSERT_EQUAL_STRING("+8613800138000",
                             humanReadablePhoneNumber(String("+8613800138000")).c_str());
}

void test_humanReadablePhoneNumber_foreign_passthrough()
{
    // +1415... doesn't match either branch -> unchanged.
    TEST_ASSERT_EQUAL_STRING("+14155551234",
                             humanReadablePhoneNumber(String("+14155551234")).c_str());
}

void test_humanReadablePhoneNumber_short_number()
{
    TEST_ASSERT_EQUAL_STRING("10086",
                             humanReadablePhoneNumber(String("10086")).c_str());
}

void test_humanReadablePhoneNumber_empty()
{
    TEST_ASSERT_EQUAL_STRING("",
                             humanReadablePhoneNumber(String("")).c_str());
}

// ---------- timestampToRFC3339 ----------

void test_timestampToRFC3339_happy_path()
{
    TEST_ASSERT_EQUAL_STRING("2024-01-15T10:30:45+08:00",
                             timestampToRFC3339(String("24/01/15,10:30:45+32")).c_str());
}

void test_timestampToRFC3339_too_short()
{
    TEST_ASSERT_EQUAL_STRING("",
                             timestampToRFC3339(String("too short")).c_str());
}

void test_timestampToRFC3339_exactly_17_chars()
{
    // Exactly 17 chars is the minimum the function accepts.
    TEST_ASSERT_EQUAL_STRING("2024-01-15T10:30:45+08:00",
                             timestampToRFC3339(String("24/01/15,10:30:45")).c_str());
}

// RFC-0169/0175: custom GMT offset (parameter is minutes)
void test_timestampToRFC3339_custom_positive_offset()
{
    TEST_ASSERT_EQUAL_STRING("2024-01-15T10:30:45+09:00",
                             timestampToRFC3339(String("24/01/15,10:30:45+32"), 540).c_str());
}

void test_timestampToRFC3339_custom_negative_offset()
{
    TEST_ASSERT_EQUAL_STRING("2024-01-15T10:30:45-05:00",
                             timestampToRFC3339(String("24/01/15,10:30:45+32"), -300).c_str());
}

void test_timestampToRFC3339_utc_zero_offset()
{
    TEST_ASSERT_EQUAL_STRING("2024-01-15T10:30:45+00:00",
                             timestampToRFC3339(String("24/01/15,10:30:45+32"), 0).c_str());
}

// RFC-0175: fractional offset (UTC+5:30)
void test_timestampToRFC3339_fractional_offset()
{
    TEST_ASSERT_EQUAL_STRING("2024-01-15T10:30:45+05:30",
                             timestampToRFC3339(String("24/01/15,10:30:45+32"), 330).c_str());
}

// ---------- Unity plumbing ----------

// RFC-0037: countSmsParts — basic cases.
void test_countSmsParts_empty_is_zero()
{
    TEST_ASSERT_EQUAL(0, sms_codec::countSmsParts(String("")));
}

void test_countSmsParts_short_ascii_is_one_part()
{
    TEST_ASSERT_EQUAL(1, sms_codec::countSmsParts(String("Hello")));
}

void test_countSmsParts_160_gsm7_is_one_part()
{
    String s;
    for (int i = 0; i < 160; i++) s += 'A';
    TEST_ASSERT_EQUAL(1, sms_codec::countSmsParts(s));
}

void test_countSmsParts_161_gsm7_is_two_parts()
{
    String s;
    for (int i = 0; i < 161; i++) s += 'A';
    TEST_ASSERT_EQUAL(2, sms_codec::countSmsParts(s));
}

void test_countSmsParts_unicode_70_is_one_part()
{
    // 70 Chinese characters = 70 UCS-2 code units = 1 part
    String s;
    for (int i = 0; i < 70; i++) {
        s += (char)(unsigned char)0xE4;
        s += (char)(unsigned char)0xBD;
        s += (char)(unsigned char)0xA0; // 你
    }
    TEST_ASSERT_EQUAL(1, sms_codec::countSmsParts(s));
}

void test_countSmsParts_unicode_71_is_two_parts()
{
    // 71 Chinese characters = 71 UCS-2 code units = 2 parts
    String s;
    for (int i = 0; i < 71; i++) {
        s += (char)(unsigned char)0xE4;
        s += (char)(unsigned char)0xBD;
        s += (char)(unsigned char)0xA0;
    }
    TEST_ASSERT_EQUAL(2, sms_codec::countSmsParts(s));
}

// RFC-0078: normalizePhoneNumber
void test_normalizePhone_strips_spaces_and_dashes()
{
    TEST_ASSERT_EQUAL_STRING("+447911123456",
        sms_codec::normalizePhoneNumber("+44 7911-123 456").c_str());
}

void test_normalizePhone_strips_parentheses()
{
    TEST_ASSERT_EQUAL_STRING("+18005550100",
        sms_codec::normalizePhoneNumber("(+1) 800-555-0100").c_str());
}

void test_normalizePhone_converts_double_zero_prefix()
{
    TEST_ASSERT_EQUAL_STRING("+447911123456",
        sms_codec::normalizePhoneNumber("0044 7911 123456").c_str());
}

void test_normalizePhone_local_format_unchanged()
{
    TEST_ASSERT_EQUAL_STRING("07911123456",
        sms_codec::normalizePhoneNumber("07911123456").c_str());
}

void test_normalizePhone_already_clean()
{
    TEST_ASSERT_EQUAL_STRING("+447911123456",
        sms_codec::normalizePhoneNumber("+447911123456").c_str());
}

void test_normalizePhone_strips_dots()
{
    TEST_ASSERT_EQUAL_STRING("+33123456789",
        sms_codec::normalizePhoneNumber("+33.1.23.45.67.89").c_str());
}

void run_sms_codec_tests()
{
    RUN_TEST(test_decodeUCS2_plain_ascii_passthrough);
    RUN_TEST(test_decodeUCS2_ascii_bytes_in_hex);
    RUN_TEST(test_decodeUCS2_chinese_bmp);
    RUN_TEST(test_decodeUCS2_surrogate_pair);
    RUN_TEST(test_decodeUCS2_odd_length_passthrough);
    RUN_TEST(test_decodeUCS2_non_hex_char_passthrough);
    RUN_TEST(test_decodeUCS2_empty_input);
    RUN_TEST(test_decodeUCS2_mixed_whitespace);

    RUN_TEST(test_parseCmgrBody_happy_path);
    RUN_TEST(test_parseCmgrBody_missing_trailing_ok);
    RUN_TEST(test_parseCmgrBody_missing_eighth_quote);
    RUN_TEST(test_parseCmgrBody_empty_body);
    RUN_TEST(test_parseCmgrBody_no_cmgr_header);

    RUN_TEST(test_humanReadablePhoneNumber_11_digit);
    RUN_TEST(test_humanReadablePhoneNumber_86_prefix_13_char);
    RUN_TEST(test_humanReadablePhoneNumber_86_prefix_14_char_passthrough);
    RUN_TEST(test_humanReadablePhoneNumber_foreign_passthrough);
    RUN_TEST(test_humanReadablePhoneNumber_short_number);
    RUN_TEST(test_humanReadablePhoneNumber_empty);

    RUN_TEST(test_timestampToRFC3339_happy_path);
    RUN_TEST(test_timestampToRFC3339_too_short);
    RUN_TEST(test_timestampToRFC3339_exactly_17_chars);
    // RFC-0169: custom GMT offset
    RUN_TEST(test_timestampToRFC3339_custom_positive_offset);
    RUN_TEST(test_timestampToRFC3339_custom_negative_offset);
    RUN_TEST(test_timestampToRFC3339_utc_zero_offset);
    RUN_TEST(test_timestampToRFC3339_fractional_offset);
    // RFC-0037: countSmsParts
    RUN_TEST(test_countSmsParts_empty_is_zero);
    RUN_TEST(test_countSmsParts_short_ascii_is_one_part);
    RUN_TEST(test_countSmsParts_160_gsm7_is_one_part);
    RUN_TEST(test_countSmsParts_161_gsm7_is_two_parts);
    RUN_TEST(test_countSmsParts_unicode_70_is_one_part);
    RUN_TEST(test_countSmsParts_unicode_71_is_two_parts);
    // RFC-0078: normalizePhoneNumber
    RUN_TEST(test_normalizePhone_strips_spaces_and_dashes);
    RUN_TEST(test_normalizePhone_strips_parentheses);
    RUN_TEST(test_normalizePhone_converts_double_zero_prefix);
    RUN_TEST(test_normalizePhone_local_format_unchanged);
    RUN_TEST(test_normalizePhone_already_clean);
    RUN_TEST(test_normalizePhone_strips_dots);
}
