// Integration tests for src/sms_handler.{h,cpp}. Uses FakeModem +
// FakeBotClient + a spy RebootFn to verify:
//   - Success path deletes the SMS and zeros the failure counter.
//   - Failure path keeps the SMS and bumps the counter.
//   - N consecutive failures triggers RebootFn exactly once.
//   - The exact AT command sequence (CMGR, CMGD, CMGL) is correct.

#include <unity.h>
#include <Arduino.h>

#include "sms_handler.h"
#include "fake_modem.h"
#include "fake_bot_client.h"

// A well-formed CMGR text-mode body that parses cleanly:
//   sender = "13800138000", content = "Hello".
static String makeCmgrResponse()
{
    return String(
        "+CMGR: \"REC UNREAD\","
        "\"00310033003800300030003100330038003000300030\","
        "\"\",\"24/01/15,10:30:45+32\"\r\n"
        "00480065006C006C006F\r\n"
        "\r\nOK\r\n");
}

// ---------- success path ----------

void test_handleSmsIndex_success_deletes_sms()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    // CMGR returns a parseable body; CMGD returns bare OK.
    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty();

    handler.handleSmsIndex(7);

    // Exact AT protocol order: CMGR then CMGD, both with index 7.
    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(2, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=7", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=7", sent[1].c_str());

    // Bot got exactly one message, containing the formatted sender + body.
    TEST_ASSERT_EQUAL(1, (int)bot.callCount());
    TEST_ASSERT_TRUE(bot.sentMessages()[0].indexOf(String("+86 138-0013-8000")) >= 0);
    TEST_ASSERT_TRUE(bot.sentMessages()[0].indexOf(String("Hello")) >= 0);
    TEST_ASSERT_TRUE(bot.sentMessages()[0].indexOf(String("2024-01-15T10:30:45+08:00")) >= 0);

    // Success -> counter zeroed, no reboot.
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

    // Bot will fail.
    bot.queueResult(false);

    modem.queueOk(makeCmgrResponse());

    handler.handleSmsIndex(3);

    // Only CMGR was sent — NO CMGD because we don't delete on failure.
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

    // Every bot call returns false.
    bot.setDefault(false);

    // Run MAX_CONSECUTIVE_FAILURES iterations. Each needs a fresh
    // CMGR response queued in the fake modem.
    for (int i = 0; i < SmsHandler::MAX_CONSECUTIVE_FAILURES; i++)
    {
        modem.queueOk(makeCmgrResponse());
        handler.handleSmsIndex(1);
    }

    // Counter should be at the threshold, reboot fired exactly once.
    TEST_ASSERT_EQUAL(SmsHandler::MAX_CONSECUTIVE_FAILURES, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(1, rebootCalls);

    // Before threshold: no reboot. Sanity: if we had stopped one short,
    // rebootCalls would be 0.
    // (This is implicitly tested by verifying "exactly once" above.)
}

// ---------- CMGR timeout / parse failure path ----------

void test_handleSmsIndex_cmgr_timeout_no_send_no_counter_change()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    // CMGR times out (code -1). Handler should bail early.
    modem.queueResponse(-1, String());

    handler.handleSmsIndex(4);

    // CMGR was sent, nothing else.
    TEST_ASSERT_EQUAL(1, (int)modem.sentCommands().size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=4", modem.sentCommands()[0].c_str());

    // Bot was never called, counter unchanged, no reboot.
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
    // CMGD will be issued for cleanup of the malformed slot.
    modem.queueOkEmpty();

    handler.handleSmsIndex(9);

    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(2, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=9", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=9", sent[1].c_str());

    // Bot never called; counter unchanged.
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

    // CMGL returns two slots, indices 1 and 2. Format mirrors what the
    // real modem emits; sweepExistingSms only reads out the numeric
    // index after "+CMGL:".
    String cmglData = String(
        "+CMGL: 1,\"REC READ\",\"001\",\"\",\"24/01/15,10:30:45+32\"\r\n"
        "BODY1\r\n"
        "+CMGL: 2,\"REC READ\",\"002\",\"\",\"24/01/15,10:30:46+32\"\r\n"
        "BODY2\r\n"
        "OK\r\n");

    modem.queueOk(cmglData);
    // Each handleSmsIndex consumes one CMGR + one CMGD.
    modem.queueOk(makeCmgrResponse()); // for idx 1 CMGR
    modem.queueOkEmpty();              // for idx 1 CMGD
    modem.queueOk(makeCmgrResponse()); // for idx 2 CMGR
    modem.queueOkEmpty();              // for idx 2 CMGD

    handler.sweepExistingSms();

    const auto &sent = modem.sentCommands();
    // Expected sequence: CMGL=ALL, CMGR=1, CMGD=1, CMGR=2, CMGD=2
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
    RUN_TEST(test_sweepExistingSms_drains_multiple_slots);
}
