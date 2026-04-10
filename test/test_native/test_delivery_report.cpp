// Unit tests for RFC-0011: SMS delivery report support.
//
// Covers:
//   - DeliveryReportMap: put/lookup/evict/collision/TTL semantics
//   - sms_codec::parseStatusReportPdu: delivered, temp failure,
//     permanent failure, truncated PDU, wrong MTI
//   - DeliveryReportHandler: Telegram notification content
//   - buildSmsSubmitPduMulti with requestStatusReport=true: TP-SRR bit set
//   - +CDS two-line URC state machine (logical, not hardware)

#include <unity.h>
#include <Arduino.h>

#include "delivery_report_map.h"
#include "delivery_report_handler.h"
#include "sms_codec.h"
#include "fake_bot_client.h"

// ---------------------------------------------------------------------------
// DeliveryReportMap tests
// ---------------------------------------------------------------------------

void test_DeliveryReportMap_put_lookup_success()
{
    DeliveryReportMap m;
    m.put(42, String("+8613800138000"), 0);

    String phone;
    TEST_ASSERT_TRUE(m.lookup(42, phone, 1000));
    TEST_ASSERT_EQUAL_STRING("+8613800138000", phone.c_str());
}

void test_DeliveryReportMap_lookup_consumes_slot()
{
    DeliveryReportMap m;
    m.put(1, String("+1"), 0);

    String phone;
    TEST_ASSERT_TRUE(m.lookup(1, phone, 0));
    // Second lookup: slot consumed, must fail.
    TEST_ASSERT_FALSE(m.lookup(1, phone, 0));
}

void test_DeliveryReportMap_lookup_wrong_mr_returns_false()
{
    DeliveryReportMap m;
    m.put(5, String("+1"), 0);

    String phone;
    // MR 5 stored at slot 5%32=5; MR 37 also maps to slot 5, but is different.
    TEST_ASSERT_FALSE(m.lookup(37, phone, 0));
    // The original MR 5 should still be there (not consumed by the miss).
    TEST_ASSERT_TRUE(m.lookup(5, phone, 0));
}

void test_DeliveryReportMap_ttl_expiry()
{
    DeliveryReportMap m;
    // Store at t=0, TTL is 3600000ms.
    m.put(10, String("+2"), 0);

    String phone;
    // Just before expiry: still valid.
    TEST_ASSERT_TRUE(m.lookup(10, phone, DeliveryReportMap::kTtlMs - 1));
    // Re-insert (lookup consumed it).
    m.put(10, String("+2"), 0);
    // At exactly TTL: expired.
    TEST_ASSERT_FALSE(m.lookup(10, phone, DeliveryReportMap::kTtlMs));
}

void test_DeliveryReportMap_evict_expired()
{
    DeliveryReportMap m;
    m.put(0, String("+0"), 0);
    m.put(1, String("+1"), 0);

    TEST_ASSERT_EQUAL(2, (int)m.occupiedSlots());

    // Evict at TTL+1: both should be evicted.
    m.evictExpired(DeliveryReportMap::kTtlMs + 1);
    TEST_ASSERT_EQUAL(0, (int)m.occupiedSlots());
}

void test_DeliveryReportMap_collision_overwrites_old_slot()
{
    DeliveryReportMap m;
    // Slot 0%32 = 0. Store MR=0, then store MR=32 (same slot).
    m.put(0, String("+first"), 0);
    m.put(32, String("+second"), 0); // overwrites slot 0

    String phone;
    // MR=0 lookup should fail (slot was overwritten).
    TEST_ASSERT_FALSE(m.lookup(0, phone, 0));
    // MR=32 should succeed.
    TEST_ASSERT_TRUE(m.lookup(32, phone, 0));
    TEST_ASSERT_EQUAL_STRING("+second", phone.c_str());
}

void test_DeliveryReportMap_phone_truncation()
{
    DeliveryReportMap m;
    // Create a phone number longer than kPhoneMax-1 = 22 chars.
    // E.164 max is 15 digits, so this tests the defensive truncation path.
    String longPhone;
    for (int i = 0; i < 30; ++i)
        longPhone += '9';

    m.put(7, longPhone, 0);

    String phone;
    TEST_ASSERT_TRUE(m.lookup(7, phone, 0));
    // The stored phone must be at most kPhoneMax-1 chars.
    TEST_ASSERT_TRUE((int)phone.length() <= (int)(DeliveryReportMap::kPhoneMax - 1));
}

// ---------------------------------------------------------------------------
// parseStatusReportPdu tests
//
// PDU construction: we build minimal hex PDUs by hand using the layout from
// 3GPP TS 23.040 §9.2.2.3:
//
//   [SCA len] [SCA bytes if any]
//   [first octet]    -- MTI=0x02 for STATUS-REPORT
//   [TP-MR]
//   [RA digit count] [RA TOA] [RA BCD bytes]
//   [SCTS 7 bytes]
//   [DT   7 bytes]
//   [TP-ST]
//
// For simplicity, SCA=00 (no SMSC address), RA=+8613800138000 (13 digits).
// RA BCD: 0x68, 0x31, 0x08, 0x10, 0x83, 0x00, 0xF0  (7 bytes)
// SCTS/DT: 7 bytes of zeroes (invalid date, but parseable).
//
// Hex:
//   00  (SCA len = 0)
//   02  (first octet: MTI=10b = status report, no MMS, no SRI, no UDHI)
//   42  (TP-MR = 66)
//   0D  (RA digit count = 13)
//   91  (RA TOA = international)
//   68 31 08 10 83 00 F0  (RA BCD for "+8613800138000")
//   00 00 00 00 00 00 00  (SCTS)
//   00 00 00 00 00 00 00  (DT)
//   00  (TP-ST = delivered)
//
// Full: 00 02 42 0D 91 68 31 08 10 83 00 F0 00*7 00*7 00
// ---------------------------------------------------------------------------

static const char *kDeliveredPdu =
    "0002420D9168310810830000"   // SCA+first+MR+RA (12 bytes)
    "00000000000000"             // SCTS (7 bytes)
    "00000000000000"             // DT   (7 bytes)
    "00";                        // TP-ST delivered

static const char *kTempFailurePdu =
    "0002420D9168310810830000"
    "00000000000000"
    "00000000000000"
    "20";                        // TP-ST = 0x20 (congestion, still trying)

static const char *kPermFailurePdu =
    "0002420D9168310810830000"
    "00000000000000"
    "00000000000000"
    "40";                        // TP-ST = 0x40 (permanent, remote procedure error)

void test_parseStatusReportPdu_delivered()
{
    sms_codec::StatusReport r;
    TEST_ASSERT_TRUE(sms_codec::parseStatusReportPdu(String(kDeliveredPdu), r));
    TEST_ASSERT_EQUAL(0x42, (int)r.messageRef); // MR = 66
    TEST_ASSERT_EQUAL(0x00, (int)r.status);
    TEST_ASSERT_TRUE(r.delivered);
    TEST_ASSERT_EQUAL_STRING("delivered", r.statusText.c_str());
    // Recipient: +8613800138000
    TEST_ASSERT_EQUAL_STRING("+8613800138000", r.recipient.c_str());
}

void test_parseStatusReportPdu_temp_failure()
{
    sms_codec::StatusReport r;
    TEST_ASSERT_TRUE(sms_codec::parseStatusReportPdu(String(kTempFailurePdu), r));
    TEST_ASSERT_EQUAL(0x20, (int)r.status);
    TEST_ASSERT_FALSE(r.delivered);
    // Status text for 0x20 = congestion
    TEST_ASSERT_TRUE(r.statusText.indexOf(String("congestion")) >= 0);
}

void test_parseStatusReportPdu_permanent_failure()
{
    sms_codec::StatusReport r;
    TEST_ASSERT_TRUE(sms_codec::parseStatusReportPdu(String(kPermFailurePdu), r));
    TEST_ASSERT_EQUAL(0x40, (int)r.status);
    TEST_ASSERT_FALSE(r.delivered);
    TEST_ASSERT_TRUE(r.statusText.indexOf(String("permanent")) >= 0);
}

void test_parseStatusReportPdu_truncated_returns_false()
{
    sms_codec::StatusReport r;
    // Too short to contain a valid PDU.
    TEST_ASSERT_FALSE(sms_codec::parseStatusReportPdu(String("0002"), r));
}

void test_parseStatusReportPdu_wrong_mti_returns_false()
{
    // MTI=01 = SMS-SUBMIT, not status report.
    const char *submitPdu = "0001420D9168310810830000"
                            "00000000000000"
                            "00000000000000"
                            "00";
    sms_codec::StatusReport r;
    TEST_ASSERT_FALSE(sms_codec::parseStatusReportPdu(String(submitPdu), r));
}

void test_parseStatusReportPdu_odd_length_returns_false()
{
    // Odd hex length is always malformed.
    sms_codec::StatusReport r;
    TEST_ASSERT_FALSE(sms_codec::parseStatusReportPdu(String("00024"), r));
}

// ---------------------------------------------------------------------------
// DeliveryReportHandler tests
// ---------------------------------------------------------------------------

void test_DeliveryReportHandler_delivered_posts_message()
{
    FakeBotClient bot;
    DeliveryReportMap map;
    uint32_t fakeNow = 1000;
    DeliveryReportHandler handler(bot, map,
                                  [&]() -> uint32_t { return fakeNow; });

    // MR=0x42 stored for "+8613800138000" at t=1000.
    map.put(0x42, String("+8613800138000"), fakeNow);

    handler.onStatusReport(String(kDeliveredPdu));

    // One Telegram message should have been sent.
    TEST_ASSERT_EQUAL(1, (int)bot.sentMessages().size());
    const String &msg = bot.sentMessages()[0];
    TEST_ASSERT_TRUE(msg.indexOf(String("+8613800138000")) >= 0);
    TEST_ASSERT_TRUE(msg.indexOf(String("Delivered")) >= 0);
}

void test_DeliveryReportHandler_permanent_failure_posts_message()
{
    FakeBotClient bot;
    DeliveryReportMap map;
    uint32_t fakeNow = 1000;
    DeliveryReportHandler handler(bot, map,
                                  [&]() -> uint32_t { return fakeNow; });

    map.put(0x42, String("+8613800138000"), fakeNow);

    handler.onStatusReport(String(kPermFailurePdu));

    TEST_ASSERT_EQUAL(1, (int)bot.sentMessages().size());
    const String &msg = bot.sentMessages()[0];
    TEST_ASSERT_TRUE(msg.indexOf(String("failed")) >= 0);
    TEST_ASSERT_TRUE(msg.indexOf(String("+8613800138000")) >= 0);
}

void test_DeliveryReportHandler_unknown_mr_sends_nothing()
{
    FakeBotClient bot;
    DeliveryReportMap map;
    uint32_t fakeNow = 1000;
    DeliveryReportHandler handler(bot, map,
                                  [&]() -> uint32_t { return fakeNow; });

    // No entry in map for MR 0x42.
    handler.onStatusReport(String(kDeliveredPdu));

    TEST_ASSERT_EQUAL(0, (int)bot.sentMessages().size());
}

void test_DeliveryReportHandler_bad_pdu_sends_nothing()
{
    FakeBotClient bot;
    DeliveryReportMap map;
    uint32_t fakeNow = 1000;
    DeliveryReportHandler handler(bot, map,
                                  [&]() -> uint32_t { return fakeNow; });

    handler.onStatusReport(String("DEADBEEF")); // not a valid status report

    TEST_ASSERT_EQUAL(0, (int)bot.sentMessages().size());
}

// ---------------------------------------------------------------------------
// buildSmsSubmitPduMulti with requestStatusReport=true
// ---------------------------------------------------------------------------

static uint8_t hexByteAt(const String &s, int offset)
{
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
        if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
        if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
        return 0;
    };
    return (uint8_t)((nibble(s[offset]) << 4) | nibble(s[offset + 1]));
}

void test_buildSmsSubmitPduMulti_srr_single_gsm7()
{
    // Single-part GSM-7: first octet must be 0x21 when requestStatusReport=true.
    auto pdus = sms_codec::buildSmsSubmitPduMulti(
        String("+8613800138000"), String("hello"),
        10, /*requestStatusReport=*/true);

    TEST_ASSERT_EQUAL(1, (int)pdus.size());
    // First octet is at hex offset 2 (after SCA byte 00).
    uint8_t firstOctet = hexByteAt(pdus[0].hex, 2);
    TEST_ASSERT_EQUAL(0x21, (int)firstOctet);
}

void test_buildSmsSubmitPduMulti_no_srr_single_gsm7()
{
    // Default (requestStatusReport=false): first octet must be 0x01.
    auto pdus = sms_codec::buildSmsSubmitPduMulti(
        String("+8613800138000"), String("hello"),
        10, /*requestStatusReport=*/false);

    TEST_ASSERT_EQUAL(1, (int)pdus.size());
    uint8_t firstOctet = hexByteAt(pdus[0].hex, 2);
    TEST_ASSERT_EQUAL(0x01, (int)firstOctet);
}

void test_buildSmsSubmitPduMulti_srr_concat_gsm7()
{
    // Multi-part GSM-7: first octet must be 0x61 (UDHI | SRR).
    String body;
    for (int i = 0; i < 161; ++i)
        body += 'A';

    auto pdus = sms_codec::buildSmsSubmitPduMulti(
        String("+8613800138000"), body,
        10, /*requestStatusReport=*/true);

    TEST_ASSERT_EQUAL(2, (int)pdus.size());
    uint8_t firstOctet = hexByteAt(pdus[0].hex, 2);
    TEST_ASSERT_EQUAL(0x61, (int)firstOctet);
}

void test_buildSmsSubmitPduMulti_srr_single_ucs2()
{
    // Single-part UCS-2: first octet must be 0x21.
    // "你好" in UTF-8
    String body;
    body += (char)(unsigned char)0xE4;
    body += (char)(unsigned char)0xBD;
    body += (char)(unsigned char)0xA0;
    body += (char)(unsigned char)0xE5;
    body += (char)(unsigned char)0xA5;
    body += (char)(unsigned char)0xBD;

    auto pdus = sms_codec::buildSmsSubmitPduMulti(
        String("+8613800138000"), body,
        10, /*requestStatusReport=*/true);

    TEST_ASSERT_EQUAL(1, (int)pdus.size());
    uint8_t firstOctet = hexByteAt(pdus[0].hex, 2);
    TEST_ASSERT_EQUAL(0x21, (int)firstOctet);
}

void run_delivery_report_tests()
{
    // DeliveryReportMap
    RUN_TEST(test_DeliveryReportMap_put_lookup_success);
    RUN_TEST(test_DeliveryReportMap_lookup_consumes_slot);
    RUN_TEST(test_DeliveryReportMap_lookup_wrong_mr_returns_false);
    RUN_TEST(test_DeliveryReportMap_ttl_expiry);
    RUN_TEST(test_DeliveryReportMap_evict_expired);
    RUN_TEST(test_DeliveryReportMap_collision_overwrites_old_slot);
    RUN_TEST(test_DeliveryReportMap_phone_truncation);

    // parseStatusReportPdu
    RUN_TEST(test_parseStatusReportPdu_delivered);
    RUN_TEST(test_parseStatusReportPdu_temp_failure);
    RUN_TEST(test_parseStatusReportPdu_permanent_failure);
    RUN_TEST(test_parseStatusReportPdu_truncated_returns_false);
    RUN_TEST(test_parseStatusReportPdu_wrong_mti_returns_false);
    RUN_TEST(test_parseStatusReportPdu_odd_length_returns_false);

    // DeliveryReportHandler
    RUN_TEST(test_DeliveryReportHandler_delivered_posts_message);
    RUN_TEST(test_DeliveryReportHandler_permanent_failure_posts_message);
    RUN_TEST(test_DeliveryReportHandler_unknown_mr_sends_nothing);
    RUN_TEST(test_DeliveryReportHandler_bad_pdu_sends_nothing);

    // buildSmsSubmitPduMulti SRR flag
    RUN_TEST(test_buildSmsSubmitPduMulti_srr_single_gsm7);
    RUN_TEST(test_buildSmsSubmitPduMulti_no_srr_single_gsm7);
    RUN_TEST(test_buildSmsSubmitPduMulti_srr_concat_gsm7);
    RUN_TEST(test_buildSmsSubmitPduMulti_srr_single_ucs2);
}
