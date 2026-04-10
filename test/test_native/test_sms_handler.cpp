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
#include "reply_target_map.h"
#include "fake_modem.h"
#include "fake_bot_client.h"
#include "fake_persist.h"
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

// Variant with a different body so two calls to handleSmsIndex don't
// trigger RFC-0061 duplicate suppression.
static String makeCmgrResponseWithBody(const char *body)
{
    PduBuildOpts opts;
    opts.sender = "13800138000";
    opts.dcs = 0x00;
    opts.bodyBytes = gsm7FromAscii(body);
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
    modem.queueOk(makeCmgrResponseWithBody("SlotA")); // different bodies: RFC-0061
    modem.queueOkEmpty();
    modem.queueOk(makeCmgrResponseWithBody("SlotB"));
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

// ---------- smsForwarded / smsFailed counters (RFC-0010) ----------

void test_smsForwarded_increments_on_success()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    TEST_ASSERT_EQUAL(0, handler.smsForwarded());
    TEST_ASSERT_EQUAL(0, handler.smsFailed());

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty();
    handler.handleSmsIndex(1);

    TEST_ASSERT_EQUAL(1, handler.smsForwarded());
    TEST_ASSERT_EQUAL(0, handler.smsFailed());

    // Second success increments again (different body to avoid RFC-0061 dedup).
    modem.queueOk(makeCmgrResponseWithBody("World"));
    modem.queueOkEmpty();
    handler.handleSmsIndex(2);

    TEST_ASSERT_EQUAL(2, handler.smsForwarded());
    TEST_ASSERT_EQUAL(0, handler.smsFailed());
}

void test_smsFailed_increments_on_telegram_failure()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    bot.queueResult(false);
    modem.queueOk(makeCmgrResponse());
    handler.handleSmsIndex(3);

    TEST_ASSERT_EQUAL(0, handler.smsForwarded());
    TEST_ASSERT_EQUAL(1, handler.smsFailed());
}

void test_smsFailed_does_not_increment_on_modem_timeout()
{
    // A CMGR timeout is not a Telegram failure — the failure counter
    // should NOT change.
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    modem.queueResponse(-1, String());
    handler.handleSmsIndex(4);

    TEST_ASSERT_EQUAL(0, handler.smsForwarded());
    TEST_ASSERT_EQUAL(0, handler.smsFailed());
}

void test_smsFailed_and_smsForwarded_independent()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    // One success (different bodies per RFC-0061: dedup checks sender+body).
    modem.queueOk(makeCmgrResponseWithBody("Msg1"));
    modem.queueOkEmpty();
    handler.handleSmsIndex(1);

    // One failure.
    bot.queueResult(false);
    modem.queueOk(makeCmgrResponseWithBody("Msg2"));
    handler.handleSmsIndex(2);

    // Another success.
    modem.queueOk(makeCmgrResponseWithBody("Msg3"));
    modem.queueOkEmpty();
    handler.handleSmsIndex(3);

    TEST_ASSERT_EQUAL(2, handler.smsForwarded());
    TEST_ASSERT_EQUAL(1, handler.smsFailed());
}

// ---------- RFC-0018: block list ----------

// Helper: build a concat fragment CMGR response.
static String makeBlockedConcatCmgr(const char *sender)
{
    PduBuildOpts opts;
    opts.sender = sender;
    opts.dcs = 0x00;
    opts.bodyBytes = gsm7FromAscii("Part1");
    opts.addConcatUdh = true;
    opts.concatRef = 0x42;
    opts.concatTotal = 2;
    opts.concatPart = 1;
    return wrapInCmgrResponse(buildPduHex(opts));
}

void test_blocked_single_part_not_forwarded_sim_slot_deleted()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    // Set up a block list containing the sender in makeCmgrResponse().
    // makeCmgrResponse() uses sender "13800138000" (international, 0x91),
    // which the PDU decoder returns as "+13800138000".
    static char blockList[1][kSmsBlockListMaxNumberLen + 1];
    strncpy(blockList[0], "+13800138000", kSmsBlockListMaxNumberLen + 1);
    handler.setBlockList(blockList, 1);

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty(); // for the CMGD

    handler.handleSmsIndex(7);

    // Bot must NOT have been called.
    TEST_ASSERT_EQUAL(0, (int)bot.callCount());

    // Commands: CMGR=7, then CMGD=7
    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(2, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=7", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=7", sent[1].c_str());

    // Failure counter must NOT be bumped.
    TEST_ASSERT_EQUAL(0, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(0, rebootCalls);
}

void test_blocked_concat_fragment_not_buffered_sim_slot_deleted()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    // Use sender "13800138000" (international 0x91) → decoded "+13800138000".
    static char blockList[1][kSmsBlockListMaxNumberLen + 1];
    strncpy(blockList[0], "+13800138000", kSmsBlockListMaxNumberLen + 1);
    handler.setBlockList(blockList, 1);

    modem.queueOk(makeBlockedConcatCmgr("13800138000"));
    modem.queueOkEmpty(); // for the CMGD

    handler.handleSmsIndex(5);

    // Bot must NOT have been called.
    TEST_ASSERT_EQUAL(0, (int)bot.callCount());

    // Fragment must NOT have entered the reassembly buffer.
    TEST_ASSERT_EQUAL(0, (int)handler.concatKeyCount());

    // Commands: CMGR=5, then CMGD=5
    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(2, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=5", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=5", sent[1].c_str());

    // Failure counter must NOT be bumped.
    TEST_ASSERT_EQUAL(0, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(0, rebootCalls);
}

// ---------- RFC-0021: runtime block list ----------

void test_runtime_blocked_single_part_not_forwarded_sim_slot_deleted()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    // Sender in makeCmgrResponse() is "+13800138000".
    static char runtimeList[1][kSmsBlockListMaxNumberLen + 1];
    strncpy(runtimeList[0], "+13800138000", kSmsBlockListMaxNumberLen + 1);
    handler.setRuntimeBlockList(runtimeList, 1);

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty(); // for the CMGD

    handler.handleSmsIndex(7);

    TEST_ASSERT_EQUAL(0, (int)bot.callCount());

    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(2, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=7", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=7", sent[1].c_str());

    TEST_ASSERT_EQUAL(0, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(0, rebootCalls);
}

void test_runtime_blocked_concat_fragment_not_buffered_sim_slot_deleted()
{
    FakeModem modem;
    FakeBotClient bot;
    int rebootCalls = 0;
    SmsHandler handler(modem, bot, [&]() { rebootCalls++; });

    static char runtimeList[1][kSmsBlockListMaxNumberLen + 1];
    strncpy(runtimeList[0], "+13800138000", kSmsBlockListMaxNumberLen + 1);
    handler.setRuntimeBlockList(runtimeList, 1);

    modem.queueOk(makeBlockedConcatCmgr("13800138000"));
    modem.queueOkEmpty(); // for the CMGD

    handler.handleSmsIndex(5);

    TEST_ASSERT_EQUAL(0, (int)bot.callCount());
    TEST_ASSERT_EQUAL(0, (int)handler.concatKeyCount());

    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(2, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGR=5", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CMGD=5", sent[1].c_str());

    TEST_ASSERT_EQUAL(0, handler.consecutiveFailures());
    TEST_ASSERT_EQUAL(0, rebootCalls);
}

// ---------- RFC-0061: duplicate suppression ----------

void test_dedup_single_suppresses_duplicate()
{
    unsigned long now = 0;
    auto clock = [&now]() -> unsigned long { return now; };
    FakeModem modem;
    FakeBotClient bot;
    SmsHandler handler(modem, bot, [&]() {}, clock);

    // First occurrence: forward normally.
    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty(); // CMGD
    handler.handleSmsIndex(1);
    TEST_ASSERT_EQUAL(1, handler.smsForwarded());
    TEST_ASSERT_EQUAL(1, (int)bot.callCount());

    // Second occurrence within the window.
    now += SmsHandler::kDedupWindowMs / 2;
    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty(); // CMGD
    handler.handleSmsIndex(2);

    // Bot must NOT have been called again.
    TEST_ASSERT_EQUAL(1, handler.smsForwarded()); // still 1
    TEST_ASSERT_EQUAL(1, (int)bot.callCount());   // still 1
    // Slot must still be deleted (not kept on SIM).
    const auto &sent = modem.sentCommands();
    // Commands: CMGR=1, CMGD=1, CMGR=2, CMGD=2
    TEST_ASSERT_EQUAL(4, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGD=2", sent[3].c_str());
    // Failure counter must NOT be bumped.
    TEST_ASSERT_EQUAL(0, handler.consecutiveFailures());
}

void test_dedup_single_allows_after_window_expires()
{
    unsigned long now = 0;
    auto clock = [&now]() -> unsigned long { return now; };
    FakeModem modem;
    FakeBotClient bot;
    SmsHandler handler(modem, bot, [&]() {}, clock);

    // First occurrence.
    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty();
    handler.handleSmsIndex(1);
    TEST_ASSERT_EQUAL(1, handler.smsForwarded());

    // Advance past the dedup window.
    now += SmsHandler::kDedupWindowMs + 1;
    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty();
    handler.handleSmsIndex(2);

    // Second occurrence should be forwarded (window expired).
    TEST_ASSERT_EQUAL(2, handler.smsForwarded());
    TEST_ASSERT_EQUAL(2, (int)bot.callCount());
}

void test_dedup_different_sender_not_suppressed()
{
    unsigned long now = 0;
    auto clock = [&now]() -> unsigned long { return now; };
    FakeModem modem;
    FakeBotClient bot;
    SmsHandler handler(modem, bot, [&]() {}, clock);

    // Build a response with different sender but same body ("Hello").
    PduBuildOpts opts;
    opts.sender = "19991112222"; // different sender
    opts.dcs = 0x00;
    opts.bodyBytes = gsm7FromAscii("Hello");
    String otherSenderCmgr = wrapInCmgrResponse(buildPduHex(opts));

    // First SMS from +13800138000 body="Hello".
    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty();
    handler.handleSmsIndex(1);

    // Second SMS from different sender, same body — should NOT be suppressed.
    modem.queueOk(otherSenderCmgr);
    modem.queueOkEmpty();
    handler.handleSmsIndex(2);

    TEST_ASSERT_EQUAL(2, handler.smsForwarded());
    TEST_ASSERT_EQUAL(2, (int)bot.callCount());
}

// ---------- RFC-0070: multi-user SMS forwarding ----------

void test_extra_recipients_receive_forwarded_sms()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    ReplyTargetMap rtm(persist);
    rtm.load();
    SmsHandler handler(modem, bot, [&]() {});
    handler.setReplyTargetMap(&rtm);

    static const int64_t extras[2] = {111, 222};
    handler.setExtraRecipients(extras, 2);

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty(); // CMGD

    handler.handleSmsIndex(1);

    // Total sends: 1 (admin via sendMessageReturningId) + 2 (extras via sendMessageToReturningId)
    TEST_ASSERT_EQUAL(3, (int)bot.callCount());
    // smsForwarded should still be 1 (each unique SMS counts once).
    TEST_ASSERT_EQUAL(1, handler.smsForwarded());

    // Verify chatIds: admin send has chatId=0 (sentinel), extras have their ids.
    const auto &msgs = bot.sentMessagesWithTarget();
    TEST_ASSERT_EQUAL(3, (int)msgs.size());
    TEST_ASSERT_EQUAL(0,   (int)msgs[0].chatId); // admin sentinel
    TEST_ASSERT_EQUAL(111, (int)msgs[1].chatId);
    TEST_ASSERT_EQUAL(222, (int)msgs[2].chatId);
    // All three should contain "Hello" (the body).
    TEST_ASSERT_TRUE(msgs[0].text.indexOf("Hello") >= 0);
    TEST_ASSERT_TRUE(msgs[1].text.indexOf("Hello") >= 0);
    TEST_ASSERT_TRUE(msgs[2].text.indexOf("Hello") >= 0);

    // RFC-0080: all three message_ids should be in the reply-target map.
    TEST_ASSERT_EQUAL(3, (int)rtm.occupiedSlots());
}

// RFC-0172: fwdTag prepended to forwarded messages
void test_fwdTag_prepended_to_forwarded_message()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    ReplyTargetMap rtm(persist);
    rtm.load();
    SmsHandler handler(modem, bot, [&]() {});
    handler.setReplyTargetMap(&rtm);
    handler.setFwdTag(String("[Home]"));

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty(); // CMGD

    handler.handleSmsIndex(1);

    TEST_ASSERT_EQUAL(1, handler.smsForwarded());
    const auto &msgs = bot.sentMessagesWithTarget();
    TEST_ASSERT_TRUE(msgs.size() > 0);
    // The message should start with "[Home] "
    TEST_ASSERT_TRUE(msgs[0].text.startsWith("[Home] "));
}

// RFC-0176: alias name prepended to forwarded message header
void test_alias_prepended_to_forwarded_message_header()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    ReplyTargetMap rtm(persist);
    rtm.load();
    SmsHandler handler(modem, bot, [&]() {});
    handler.setReplyTargetMap(&rtm);
    // makeCmgrResponse uses sender "13800138000" with TOA 0x91 (international),
    // so pdu.sender == "+13800138000".
    handler.setAliasFn([](const String &phone) -> String {
        if (phone == "+13800138000") return String("alice");
        return String();
    });

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty(); // CMGD

    handler.handleSmsIndex(1);

    TEST_ASSERT_EQUAL(1, handler.smsForwarded());
    const auto &msgs = bot.sentMessagesWithTarget();
    TEST_ASSERT_TRUE(msgs.size() > 0);
    // Header should contain "alice (" before the phone number.
    TEST_ASSERT_TRUE(msgs[0].text.indexOf("alice (") >= 0);
    // Raw number should still appear (inside parentheses).
    TEST_ASSERT_TRUE(msgs[0].text.indexOf("+13800138000") >= 0);
}

// RFC-0181: previewFormat respects current settings
void test_previewFormat_uses_fwdTag_and_gmtOffset()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    ReplyTargetMap rtm(persist);
    rtm.load();
    SmsHandler handler(modem, bot, [&]() {});
    handler.setFwdTag(String("[Test]"));
    handler.setGmtOffsetMinutes(540); // UTC+9

    String result = handler.previewFormat(
        String("+10000000000"),
        String("24/01/15,10:30:45+32"),
        String("Hello")
    );

    TEST_ASSERT_TRUE(result.startsWith("[Test]"));
    TEST_ASSERT_TRUE(result.indexOf("+09:00") >= 0);
    TEST_ASSERT_TRUE(result.indexOf("Hello") >= 0);
}

// ---------- RFC-0190: SMS age filter ----------

// The test PDU timestamp encodes 2024-01-15 10:30:45 GMT+8 = 02:30:45 UTC.
// pduTimestampToUnix("24/01/15,10:30:45+32") should equal 1705285845.
static constexpr long kPduUnix = 1705285845L;

// Wall clock returns a time 25 hours after the PDU => age 25h.
// With filter = 24h the SMS should be skipped.
static void test_smsAgeFilter_skips_old_sms()
{
    FakeModem modem;
    FakeBotClient bot;
    SmsHandler handler(modem, bot, [&]() {});
    handler.setMaxSmsAgeHours(24);
    // Inject wall clock: 25 hours after PDU
    handler.setWallClockFn([&]() -> long { return kPduUnix + 25 * 3600L; });

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty(); // CMGD response

    handler.handleSmsIndex(1);

    // No Telegram message sent — SMS was filtered.
    TEST_ASSERT_EQUAL_INT(0, (int)bot.sentMessages().size());
    // CMGD was called to delete the stale SMS.
    const auto &cmds = modem.sentCommands();
    bool deleted = false;
    for (const auto &cmd : cmds)
        if (cmd.indexOf("+CMGD=") >= 0) deleted = true;
    TEST_ASSERT_TRUE(deleted);
}

// Wall clock returns a time 1 hour after the PDU => age 1h.
// With filter = 24h the SMS should be forwarded normally.
static void test_smsAgeFilter_forwards_recent_sms()
{
    FakeModem modem;
    FakeBotClient bot;
    SmsHandler handler(modem, bot, [&]() {});
    handler.setMaxSmsAgeHours(24);
    // Inject wall clock: only 1 hour after PDU
    handler.setWallClockFn([&]() -> long { return kPduUnix + 1 * 3600L; });

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty(); // CMGD response on success

    handler.handleSmsIndex(1);

    // SMS was within the filter window — should be forwarded.
    TEST_ASSERT_EQUAL_INT(1, (int)bot.sentMessages().size());
}

// filter = 0 means disabled; any age is forwarded.
static void test_smsAgeFilter_disabled_forwards_very_old_sms()
{
    FakeModem modem;
    FakeBotClient bot;
    SmsHandler handler(modem, bot, [&]() {});
    handler.setMaxSmsAgeHours(0); // disabled
    // Wall clock far in the future — would normally trigger the filter.
    handler.setWallClockFn([&]() -> long { return kPduUnix + 9000 * 3600L; });

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty();

    handler.handleSmsIndex(1);

    TEST_ASSERT_EQUAL_INT(1, (int)bot.sentMessages().size());
}

// If wall clock returns < 1e9 (NTP not synced), the filter is bypassed.
static void test_smsAgeFilter_bypassed_when_no_ntp()
{
    FakeModem modem;
    FakeBotClient bot;
    SmsHandler handler(modem, bot, [&]() {});
    handler.setMaxSmsAgeHours(1);
    // Wall clock returns 0 (no NTP sync)
    handler.setWallClockFn([&]() -> long { return 0L; });

    modem.queueOk(makeCmgrResponse());
    modem.queueOkEmpty();

    handler.handleSmsIndex(1);

    // Filter bypassed — SMS forwarded.
    TEST_ASSERT_EQUAL_INT(1, (int)bot.sentMessages().size());
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
    RUN_TEST(test_smsForwarded_increments_on_success);
    RUN_TEST(test_smsFailed_increments_on_telegram_failure);
    RUN_TEST(test_smsFailed_does_not_increment_on_modem_timeout);
    RUN_TEST(test_smsFailed_and_smsForwarded_independent);
    RUN_TEST(test_blocked_single_part_not_forwarded_sim_slot_deleted);
    RUN_TEST(test_blocked_concat_fragment_not_buffered_sim_slot_deleted);
    // RFC-0021: runtime block list
    RUN_TEST(test_runtime_blocked_single_part_not_forwarded_sim_slot_deleted);
    RUN_TEST(test_runtime_blocked_concat_fragment_not_buffered_sim_slot_deleted);
    // RFC-0061: duplicate suppression
    RUN_TEST(test_dedup_single_suppresses_duplicate);
    RUN_TEST(test_dedup_single_allows_after_window_expires);
    RUN_TEST(test_dedup_different_sender_not_suppressed);
    // RFC-0070: multi-user forwarding
    RUN_TEST(test_extra_recipients_receive_forwarded_sms);
    // RFC-0172: forward tag
    RUN_TEST(test_fwdTag_prepended_to_forwarded_message);
    // RFC-0176: alias in header
    RUN_TEST(test_alias_prepended_to_forwarded_message_header);
    // RFC-0181: previewFormat
    RUN_TEST(test_previewFormat_uses_fwdTag_and_gmtOffset);
    // RFC-0190: SMS age filter
    RUN_TEST(test_smsAgeFilter_skips_old_sms);
    RUN_TEST(test_smsAgeFilter_forwards_recent_sms);
    RUN_TEST(test_smsAgeFilter_disabled_forwards_very_old_sms);
    RUN_TEST(test_smsAgeFilter_bypassed_when_no_ntp);
}
