// Integration tests for src/sms_handler.{h,cpp}. Uses FakeModem +
// FakeBotClient + a spy RebootFn to verify:
//   - Success path deletes the SMS and zeros the failure counter.
//   - Failure path keeps the SMS and bumps the counter.
//   - N consecutive failures triggers RebootFn exactly once.
//   - The exact AT command sequence (CMGR, CMGD, CMGL) is correct.
//
// Now in PDU mode (RFC-0002): test fixtures build SMS-DELIVER PDUs
// programmatically via pdu_test_helpers.h and wrap them in the PDU-mode
// +CMGR response envelope.

#include <unity.h>
#include <Arduino.h>

#include "sms_handler.h"
#include "fake_modem.h"
#include "fake_bot_client.h"
#include "pdu_test_helpers.h"

using pdu_test::buildPduHex;
using pdu_test::gsm7FromAscii;
using pdu_test::PduBuildOpts;
using pdu_test::wrapInCmgrResponse;

// A well-formed single-part GSM-7 CMGR response whose content decodes
// to "Hello" from sender "+13800138000".
static String makeCmgrResponse()
{
    PduBuildOpts opts;
    opts.sender = "13800138000";
    opts.dcs = 0x00;
    opts.bodyBytes = gsm7FromAscii("Hello");
    return wrapInCmgrResponse(buildPduHex(opts));
}

// ---------- success path ----------

void test_handleSmsIndex_success_deletes_sms()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty();

    handler.handleSmsIndex(7);

    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(2, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=7", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=7", sent[1].c_str());

    TEST_ASSERT_EQUAL(1, (int)bot.callCount());
    // encodeTpOa prepended '+' because we marked the number as
    // international (0x91). humanReadablePhoneNumber doesn't match
    // the +86/11-digit forms for +13800138000, so it passes through.
    TEST_ASSERT_TRUE(bot.sentMessages()[0].indexOf(String("+13800138000")) >= 0);
    TEST_ASSERT_TRUE(bot.sentMessages()[0].indexOf(String("Hello")) >= 0);
    TEST_ASSERT_TRUE(bot.sentMessages()[0].indexOf(String("2024-01-15T10:30:45+08:00")) >= 0);

    TEST_ASSERT_EQUAL(0, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(0, rebootCalls);
}

// ---------- failure path ----------

void test_handleSmsIndex_failure_keeps_sms_and_bumps_counter()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    bot.queueResult(false);

    modem.queueOk(makeCmgrResponse());

    handler.handleSmsIndex(3);

    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(1, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=3", sent[0].c_str());

    TEST_ASSERT_EQUAL(1, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(0, rebootCalls);
}

// ---------- reboot threshold ----------

void test_handleSmsIndex_reboot_after_max_consecutive_failures()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    bot.setDefault(false);

    for (int i = 0; i < SmsHandler::MAX_CONSECUTIVE_FAILURES; i++)
    {
        modem.queueOk(makeCmgrResponse());
        handler.handleSmsIndex(1);
    }

    TEST_ASSERT_EQUAL(SmsHandler::MAX_CONSECUTIVE_FAILURES, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(1, rebootCalls);
}

// ---------- CMGR timeout / parse failure path ----------

void test_handleSmsIndex_cmgr_timeout_no_send_no_counter_change()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    modem.queueResponse(-1, String());

    handler.handleSmsIndex(4);

    TEST_ASSERT_EQUAL(1, (int)modem.sentCommands().size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=4", modem.sentCommands()[0].c_str());

    TEST_ASSERT_EQUAL(0, (int)bot.callCount());
    TEST_ASSERT_EQUAL(0, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(0, rebootCalls);
}

void test_handleSmsIndex_unparseable_body_deletes_slot()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    // CMGR returns OK but the body is garbage (no "+CMGR:" header).
    modem.queueOk(String("garbage\r\nOK\r\n"));
    modem.queueOkEmpty();

    handler.handleSmsIndex(9);

    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(2, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=9", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=9", sent[1].c_str());

    TEST_ASSERT_EQUAL(0, (int)bot.callCount());
    TEST_ASSERT_EQUAL(0, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(0, rebootCalls);
}

// Extra (RFC-0002): a CMGR response whose body is structurally valid
// on the envelope level but whose PDU hex is truncated. The handler
// should wipe the slot so we don't loop on a broken PDU.
void test_handleSmsIndex_malformed_pdu_deletes_slot()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    // Valid CMGR envelope but the PDU bytes cut off inside the TP-OA.
    modem.queueOk(String("+CMGR: 0,,4\r\n00040B91\r\n\r\nOK\r\n"));
    modem.queueOkEmpty();

    handler.handleSmsIndex(5);

    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(2, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=5", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=5", sent[1].c_str());

    TEST_ASSERT_EQUAL(0, (int)bot.callCount());
    TEST_ASSERT_EQUAL(0, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(0, rebootCalls);
}

// ---------- sweepExistingSms ----------

void test_sweepExistingSms_drains_multiple_slots()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    // In PDU mode the CMGL response carries +CMGL: <idx>,<stat>,... lines.
    // sweepExistingSms only reads the numeric index, so any textual
    // filler is fine.
    String cmglData = String(
        "+CMGL: 1,0,,5\r\n"
        "SOMEPDUHEX\r\n"
        "+CMGL: 2,0,,5\r\n"
        "SOMEPDUHEX\r\n"
        "OK\r\n");

    modem.queueOk(cmglData);
    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty();
    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty();

    handler.sweepExistingSms();

    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(5, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGL=\"ALL\"", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGR=1", sent[1].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=1", sent[2].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGR=2", sent[3].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=2", sent[4].c_str());

    TEST_ASSERT_EQUAL(2, (int)bot.callCount());
}

// ---------- Unity plumbing ----------

void run_sms_handler_tests()
{
    RUN_TEST(test_handleSmsIndex_success_deletes_sms);
    RUN_TEST(test_handleSmsIndex_failure_keeps_sms_and_bumps_counter);
    RUN_TEST(test_handleSmsIndex_reboot_after_max_consecutive_failures);
    RUN_TEST(test_handleSmsIndex_cmgr_timeout_no_send_no_counter_change);
    RUN_TEST(test_handleSmsIndex_unparseable_body_deletes_slot);
    RUN_TEST(test_handleSmsIndex_malformed_pdu_deletes_slot);
    RUN_TEST(test_sweepExistingSms_drains_multiple_slots);
}
