// Unit tests for src/telegram_poller.{h,cpp}.
//
// Coverage (RFC-0003):
//   - Long poll happy path: update with reply_to_message_id matching
//     a slot in the ring buffer triggers an SMS send + watermark advance.
//   - Authorization reject: from_id != allow list -> drop, watermark
//     still advances.
//   - Stale slot reject: ring buffer has been overwritten -> error
//     reply, no SMS, watermark advances.
//   - Non-ASCII body bail: SmsSender returns false, error reply
//     posted, watermark advances.
//   - Update with no message: skipped via valid=false flag, watermark
//     still advances past it.
//   - Pollers respect rate limit: tick() within kPollIntervalMs is a
//     no-op.
//   - Persistence across "restart": destroy + recreate poller; the
//     new instance reads the watermark from FakePersist and does NOT
//     re-process old updates (we feed it the same script, it asks
//     for offset > saved id).

#include <unity.h>
#include <Arduino.h>

#include "telegram_poller.h"
#include "reply_target_map.h"
#include "sms_sender.h"
#include "fake_modem.h"
#include "fake_bot_client.h"
#include "fake_persist.h"

namespace {

constexpr int64_t kAllowedFromId = 12345;

struct ClockFixture
{
    uint32_t nowMs = 0;
};

// Build a single update.
TelegramUpdate makeUpdate(int32_t updateId, int64_t fromId,
                          int32_t replyToId, const char *text)
{
    TelegramUpdate u;
    u.updateId = updateId;
    u.fromId = fromId;
    u.replyToMessageId = replyToId;
    u.text = String(text);
    u.valid = true;
    return u;
}

// Default auth predicate: matches kAllowedFromId.
auto allowedAuth = [](int64_t fromId) -> bool {
    return fromId == kAllowedFromId;
};

// Allow-anything auth.
auto allowAllAuth = [](int64_t /*fromId*/) -> bool { return true; };

} // namespace

void test_TelegramPoller_happy_path_routes_reply_to_phone()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();
    rtm.put(42, String("+8613800138000"));

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    // Modem will accept the SMS, then we restore PDU mode.
    modem.queueOkEmpty(); // for +CMGF=0 after sendSMS

    bot.queueUpdateBatch({makeUpdate(101, kAllowedFromId, 42, "hi there")});

    poller.tick();

    // Exactly one SMS sent to the right number.
    TEST_ASSERT_EQUAL(1, (int)modem.smsSendCalls().size());
    TEST_ASSERT_EQUAL_STRING("+8613800138000", modem.smsSendCalls()[0].number.c_str());
    TEST_ASSERT_EQUAL_STRING("hi there", modem.smsSendCalls()[0].text.c_str());

    // Watermark advanced.
    TEST_ASSERT_EQUAL(101, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(101, persist.loadLastUpdateId());

    // Confirmation reply was posted.
    bool sawConfirmation = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Reply sent to")) >= 0)
        {
            sawConfirmation = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawConfirmation);
}

void test_TelegramPoller_unauthorized_drops_and_advances()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();
    rtm.put(42, String("+8613800138000"));

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    // Different from_id — not allowed.
    bot.queueUpdateBatch({makeUpdate(50, 99999, 42, "hi")});

    poller.tick();

    // No SMS sent.
    TEST_ASSERT_EQUAL(0, (int)modem.smsSendCalls().size());
    // Watermark still advanced.
    TEST_ASSERT_EQUAL(50, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(50, persist.loadLastUpdateId());
    // No bot messages sent (we don't even acknowledge unauthorized
    // attempts to avoid being a probe oracle).
    TEST_ASSERT_EQUAL(0, (int)bot.callCount());
}

void test_TelegramPoller_stale_slot_rejects_with_error_reply()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();
    // Slot for msg_id=42 has been overwritten by msg_id=242.
    rtm.put(242, String("+8613800138000"));

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    // Reply to old msg 42 — slot now belongs to 242, lookup fails.
    bot.queueUpdateBatch({makeUpdate(7, kAllowedFromId, 42, "hi")});

    poller.tick();

    // No SMS sent.
    TEST_ASSERT_EQUAL(0, (int)modem.smsSendCalls().size());
    // Watermark advanced.
    TEST_ASSERT_EQUAL(7, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(7, persist.loadLastUpdateId());
    // Error reply was posted.
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("expired")) >= 0)
        {
            sawError = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawError);
}

void test_TelegramPoller_non_ascii_body_bails_with_error_reply()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();
    rtm.put(42, String("+8613800138000"));

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    // "你好" in UTF-8.
    String body;
    body += (char)0xE4;
    body += (char)0xBD;
    body += (char)0xA0;
    body += (char)0xE5;
    body += (char)0xA5;
    body += (char)0xBD;
    TelegramUpdate u = makeUpdate(33, kAllowedFromId, 42, "");
    u.text = body;
    bot.queueUpdateBatch({u});

    poller.tick();

    // No SMS attempted (the ASCII gate happens before sendSMS).
    TEST_ASSERT_EQUAL(0, (int)modem.smsSendCalls().size());

    // Watermark advanced.
    TEST_ASSERT_EQUAL(33, poller.lastUpdateId());

    // Error reply mentions ASCII / RFC-0002.
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("non-ASCII")) >= 0)
        {
            sawError = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawError);
}

void test_TelegramPoller_invalid_update_advances_watermark()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    // Update with valid=false (e.g. channel_post that the JSON parser
    // saw and recorded the update_id for, but couldn't extract a
    // message body from). RFC-0003 §5: drop, advance.
    TelegramUpdate u;
    u.updateId = 999;
    u.valid = false;
    bot.queueUpdateBatch({u});

    poller.tick();

    TEST_ASSERT_EQUAL(0, (int)modem.smsSendCalls().size());
    TEST_ASSERT_EQUAL(999, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(999, persist.loadLastUpdateId());
}

void test_TelegramPoller_no_reply_to_message_id_drops_with_help()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(11, kAllowedFromId, 0, "hello bot")});

    poller.tick();

    TEST_ASSERT_EQUAL(0, (int)modem.smsSendCalls().size());
    TEST_ASSERT_EQUAL(11, poller.lastUpdateId());
    bool sawHelp = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Reply to a forwarded SMS")) >= 0)
        {
            sawHelp = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawHelp);
}

void test_TelegramPoller_rate_limit_one_poll_per_interval()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    clk.nowMs = 1000;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
    poller.begin();

    // First tick should poll (firstPollDone_ = false initially).
    poller.tick();
    TEST_ASSERT_EQUAL(1, bot.pollCallCount());

    // Immediate retick should NOT poll.
    poller.tick();
    TEST_ASSERT_EQUAL(1, bot.pollCallCount());

    // Advance just under the interval.
    clk.nowMs += TelegramPoller::kPollIntervalMs - 1;
    poller.tick();
    TEST_ASSERT_EQUAL(1, bot.pollCallCount());

    // Cross the interval — next tick polls again.
    clk.nowMs += 2;
    poller.tick();
    TEST_ASSERT_EQUAL(2, bot.pollCallCount());
}

void test_TelegramPoller_transport_failure_does_not_advance()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
    poller.begin();

    bot.queuePollFailure();
    poller.tick();
    TEST_ASSERT_EQUAL(0, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(0, persist.loadLastUpdateId());
}

void test_TelegramPoller_persistence_across_restart_does_not_replay()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();
    rtm.put(42, String("+861"));

    ClockFixture clk;

    {
        TelegramPoller p1(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
        p1.begin();
        modem.queueOkEmpty(); // PDU restore after sendSMS
        bot.queueUpdateBatch({makeUpdate(50, kAllowedFromId, 42, "hi")});
        p1.tick();
        TEST_ASSERT_EQUAL(50, p1.lastUpdateId());
        TEST_ASSERT_EQUAL(50, persist.loadLastUpdateId());
        TEST_ASSERT_EQUAL(1, (int)modem.smsSendCalls().size());
    }

    // "Restart": new poller from same persist. Even if the bot
    // happened to return the same update again, the poller's
    // in-RAM watermark loaded from persist would skip it.
    {
        TelegramPoller p2(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
        p2.begin();
        TEST_ASSERT_EQUAL(50, p2.lastUpdateId());

        // Replay the same update id (simulating Telegram echoing it
        // back if our offset were wrong). The poller's
        // "<= lastUpdateId_" guard skips it.
        bot.queueUpdateBatch({makeUpdate(50, kAllowedFromId, 42, "hi again")});
        p2.tick();
        // No new SMS.
        TEST_ASSERT_EQUAL(1, (int)modem.smsSendCalls().size());
        // Watermark unchanged.
        TEST_ASSERT_EQUAL(50, p2.lastUpdateId());
    }
}

void test_TelegramPoller_offset_passed_to_bot_uses_watermark()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);

    // Pre-seed the persist watermark BEFORE begin() so the poller
    // picks it up at startup.
    persist.saveLastUpdateId(77);
    poller.begin();

    poller.tick();
    TEST_ASSERT_EQUAL(77, bot.lastPollOffset());
}

void test_TelegramPoller_multiple_updates_in_one_batch()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();
    rtm.put(10, String("+86A"));
    rtm.put(20, String("+86B"));

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
    poller.begin();

    // Two PDU restores expected.
    modem.queueOkEmpty();
    modem.queueOkEmpty();

    bot.queueUpdateBatch({
        makeUpdate(100, kAllowedFromId, 10, "first"),
        makeUpdate(101, kAllowedFromId, 20, "second"),
    });

    poller.tick();

    TEST_ASSERT_EQUAL(2, (int)modem.smsSendCalls().size());
    TEST_ASSERT_EQUAL_STRING("+86A", modem.smsSendCalls()[0].number.c_str());
    TEST_ASSERT_EQUAL_STRING("first", modem.smsSendCalls()[0].text.c_str());
    TEST_ASSERT_EQUAL_STRING("+86B", modem.smsSendCalls()[1].number.c_str());
    TEST_ASSERT_EQUAL_STRING("second", modem.smsSendCalls()[1].text.c_str());
    TEST_ASSERT_EQUAL(101, poller.lastUpdateId());
}

void run_telegram_poller_tests()
{
    RUN_TEST(test_TelegramPoller_happy_path_routes_reply_to_phone);
    RUN_TEST(test_TelegramPoller_unauthorized_drops_and_advances);
    RUN_TEST(test_TelegramPoller_stale_slot_rejects_with_error_reply);
    RUN_TEST(test_TelegramPoller_non_ascii_body_bails_with_error_reply);
    RUN_TEST(test_TelegramPoller_invalid_update_advances_watermark);
    RUN_TEST(test_TelegramPoller_no_reply_to_message_id_drops_with_help);
    RUN_TEST(test_TelegramPoller_rate_limit_one_poll_per_interval);
    RUN_TEST(test_TelegramPoller_transport_failure_does_not_advance);
    RUN_TEST(test_TelegramPoller_persistence_across_restart_does_not_replay);
    RUN_TEST(test_TelegramPoller_offset_passed_to_bot_uses_watermark);
    RUN_TEST(test_TelegramPoller_multiple_updates_in_one_batch);
}
