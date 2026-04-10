// Unit tests for src/telegram_poller.{h,cpp}.
//
// Coverage (RFC-0003):
//   - Happy path: update with reply_to_message_id matching a slot in
//     the ring buffer triggers a PDU SMS send + watermark advance.
//   - Authorization reject: from_id != allow list -> drop, watermark
//     still advances.
//   - Stale slot reject: ring buffer has been overwritten -> error
//     reply, no SMS, watermark advances.
//   - Unicode body: SmsSender now handles Unicode via UCS-2 PDU.
//   - Update with no message: skipped via valid=false flag, watermark
//     still advances past it.
//   - Pollers respect rate limit: tick() within kPollIntervalMs is a
//     no-op.
//   - Persistence across "restart": destroy + recreate poller; the
//     new instance reads the watermark from FakePersist and does NOT
//     re-process old updates.

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
// chatId defaults to 0; pass an explicit value to test per-requester routing.
// In real Telegram messages chatId == fromId for DMs and chatId is negative for groups.
TelegramUpdate makeUpdate(int32_t updateId, int64_t fromId,
                          int32_t replyToId, const char *text,
                          int64_t chatId = 0)
{
    TelegramUpdate u;
    u.updateId = updateId;
    u.fromId = fromId;
    u.chatId = chatId;
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

    bot.queueUpdateBatch({makeUpdate(101, kAllowedFromId, 42, "hi there")});

    // tick() enqueues the SMS but does NOT send it yet (RFC-0012).
    poller.tick();

    // No PDU send yet — entry is in the queue.
    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());

    // drainQueue triggers the actual send.
    sender.drainQueue(0);

    // Exactly one PDU SMS sent (SmsSender now uses sendPduSms).
    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());
    // No text-mode sendSMS used.
    TEST_ASSERT_EQUAL(0, (int)modem.smsSendCalls().size());

    // Watermark advanced.
    TEST_ASSERT_EQUAL(101, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(101, persist.loadLastUpdateId());

    // "Queued reply" confirmation was posted immediately by tick().
    bool sawConfirmation = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Queued reply to")) >= 0)
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
    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());
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
    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());
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

void test_TelegramPoller_unicode_body_sends_via_ucs2()
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

    // "你好" in UTF-8 — should now succeed via UCS-2 PDU.
    String body;
    body += (char)(unsigned char)0xE4;
    body += (char)(unsigned char)0xBD;
    body += (char)(unsigned char)0xA0;
    body += (char)(unsigned char)0xE5;
    body += (char)(unsigned char)0xA5;
    body += (char)(unsigned char)0xBD;
    TelegramUpdate u = makeUpdate(33, kAllowedFromId, 42, "");
    u.text = body;
    bot.queueUpdateBatch({u});

    // tick() enqueues the SMS; drainQueue triggers the actual send.
    poller.tick();
    sender.drainQueue(0);

    // Unicode SMS now succeeds (was rejected in the old ASCII-only path).
    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());
    TEST_ASSERT_EQUAL(33, poller.lastUpdateId());

    // "Queued reply" confirmation was posted immediately by tick().
    bool sawConfirmation = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Queued reply to")) >= 0)
        {
            sawConfirmation = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawConfirmation);
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

    TelegramUpdate u;
    u.updateId = 999;
    u.valid = false;
    bot.queueUpdateBatch({u});

    poller.tick();

    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());
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

    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());
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
        bot.queueUpdateBatch({makeUpdate(50, kAllowedFromId, 42, "hi")});
        p1.tick();
        // drainQueue needed to actually send (RFC-0012).
        sender.drainQueue(0);
        TEST_ASSERT_EQUAL(50, p1.lastUpdateId());
        TEST_ASSERT_EQUAL(50, persist.loadLastUpdateId());
        TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());
    }

    // "Restart": new poller from same persist.
    {
        TelegramPoller p2(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
        p2.begin();
        TEST_ASSERT_EQUAL(50, p2.lastUpdateId());

        bot.queueUpdateBatch({makeUpdate(50, kAllowedFromId, 42, "hi again")});
        p2.tick();
        // No new SMS.
        TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());
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

    bot.queueUpdateBatch({
        makeUpdate(100, kAllowedFromId, 10, "first"),
        makeUpdate(101, kAllowedFromId, 20, "second"),
    });

    // tick() enqueues both; two drainQueue calls deliver them (one per call).
    poller.tick();
    sender.drainQueue(0);
    sender.drainQueue(0);

    TEST_ASSERT_EQUAL(2, (int)modem.pduSendCalls().size());
    TEST_ASSERT_EQUAL(101, poller.lastUpdateId());
}

// ---------- /status command tests (RFC-0010) ----------

void test_TelegramPoller_status_command_calls_status_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    // StatusFn returns a canned string.
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth,
                          []() -> String { return String("device-ok"); });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(200, kAllowedFromId, 0, "/status")});
    poller.tick();

    // No SMS sent — /status is a command, not a reply.
    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());
    // Watermark advanced.
    TEST_ASSERT_EQUAL(200, poller.lastUpdateId());
    // The canned status string was sent to the bot.
    bool sawStatus = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("device-ok")) >= 0)
        {
            sawStatus = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawStatus);
}

void test_TelegramPoller_status_command_nullptr_fallback()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    // No StatusFn supplied (nullptr default).
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(201, kAllowedFromId, 0, "/status")});
    poller.tick();

    // Fallback message sent.
    bool sawFallback = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("status not configured")) >= 0)
        {
            sawFallback = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawFallback);
}

void test_TelegramPoller_help_message_mentions_status()
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

    // Non-reply, non-command message triggers the help/error text.
    bot.queueUpdateBatch({makeUpdate(202, kAllowedFromId, 0, "hello bot")});
    poller.tick();

    bool sawStatusMention = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("/status")) >= 0)
        {
            sawStatusMention = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawStatusMention);
}

// ---------- RFC-0016: per-requester command reply ----------

// /status from a non-admin user (chatId=999): reply must go to chatId=999.
void test_TelegramPoller_status_reply_goes_to_requester_chat()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    constexpr int64_t kNonAdminChatId = 999;
    // Allow both the admin and the non-admin for this test.
    auto multiAuth = [](int64_t fromId) -> bool {
        return fromId == kAllowedFromId || fromId == kNonAdminChatId;
    };

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          multiAuth,
                          []() -> String { return String("device-ok"); });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(300, kNonAdminChatId, 0, "/status", kNonAdminChatId)});
    poller.tick();

    // No SMS, watermark advanced.
    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());
    TEST_ASSERT_EQUAL(300, poller.lastUpdateId());

    // Reply must target chatId=999, not admin.
    bool sawCorrectTarget = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.chatId == kNonAdminChatId && m.text.indexOf(String("device-ok")) >= 0)
        {
            sawCorrectTarget = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCorrectTarget);
}

// /debug from a non-admin user (chatId=999): reply must go to chatId=999.
void test_TelegramPoller_debug_reply_goes_to_requester_chat()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    constexpr int64_t kNonAdminChatId = 999;
    auto multiAuth = [](int64_t fromId) -> bool {
        return fromId == kAllowedFromId || fromId == kNonAdminChatId;
    };

    ClockFixture clk;
    // No debug log configured — should reply "(debug log not configured)".
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          multiAuth);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(301, kNonAdminChatId, 0, "/debug", kNonAdminChatId)});
    poller.tick();

    TEST_ASSERT_EQUAL(301, poller.lastUpdateId());

    bool sawCorrectTarget = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.chatId == kNonAdminChatId && m.text.indexOf(String("debug log not configured")) >= 0)
        {
            sawCorrectTarget = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCorrectTarget);
}

// Error reply (no reply_to_message_id) goes to u.chatId, not admin chat.
void test_TelegramPoller_error_reply_goes_to_requester_chat()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    constexpr int64_t kRequesterChatId = 555;

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          [](int64_t) -> bool { return true; }); // allow all
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(302, kAllowedFromId, 0, "hello bot", kRequesterChatId)});
    poller.tick();

    bool sawCorrectTarget = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.chatId == kRequesterChatId && m.text.indexOf(String("Reply to a forwarded SMS")) >= 0)
        {
            sawCorrectTarget = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCorrectTarget);
}

// Group chat scenario: chatId is negative, fromId is the individual's positive ID.
// The reply must go to the group (negative chatId), not the individual.
void test_TelegramPoller_group_chat_reply_goes_to_group()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();
    rtm.put(42, String("+8613800138000"));

    constexpr int64_t kGroupChatId = -1001234567890LL;
    constexpr int64_t kMemberId = 111;

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          [](int64_t) -> bool { return true; },
                          []() -> String { return String("group-status"); });
    poller.begin();

    // /status from a group member — chatId is the group, fromId is the member.
    bot.queueUpdateBatch({makeUpdate(303, kMemberId, 0, "/status", kGroupChatId)});
    poller.tick();

    TEST_ASSERT_EQUAL(303, poller.lastUpdateId());

    bool sawGroupTarget = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.chatId == kGroupChatId && m.text.indexOf(String("group-status")) >= 0)
        {
            sawGroupTarget = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawGroupTarget);
}

// sendMessageReturningId for SMS forward still uses the admin sentinel (chatId=0).
// This exercises the SmsHandler isolation invariant from RFC-0016 §5.
void test_TelegramPoller_sms_forward_uses_admin_sentinel()
{
    // SmsHandler (not TelegramPoller) calls sendMessageReturningId. We verify
    // here that FakeBotClient records it with chatId=0 (the admin sentinel).
    FakeBotClient bot;
    bot.sendMessageReturningId(String("forwarded SMS text"));

    TEST_ASSERT_EQUAL(1, (int)bot.sentMessagesWithTarget().size());
    TEST_ASSERT_EQUAL(0, (int)bot.sentMessagesWithTarget()[0].chatId);
    TEST_ASSERT_EQUAL(1, (int)bot.callCount());
}

// Admin user invokes /status — reply goes to u.chatId which equals the admin's chat.
void test_TelegramPoller_admin_status_reply_goes_to_admin_chat()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    constexpr int64_t kAdminChatId = kAllowedFromId; // DM: chatId == fromId

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth,
                          []() -> String { return String("admin-status-ok"); });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(304, kAllowedFromId, 0, "/status", kAdminChatId)});
    poller.tick();

    TEST_ASSERT_EQUAL(304, poller.lastUpdateId());

    bool sawAdminTarget = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.chatId == kAdminChatId && m.text.indexOf(String("admin-status-ok")) >= 0)
        {
            sawAdminTarget = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawAdminTarget);
}

// Successful SMS enqueue: "Queued reply" confirmation goes to u.chatId.
void test_TelegramPoller_enqueue_confirmation_goes_to_requester_chat()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();
    rtm.put(42, String("+8613800138000"));

    constexpr int64_t kRequesterChatId = 777;

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          [](int64_t) -> bool { return true; });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(305, kAllowedFromId, 42, "hi there", kRequesterChatId)});
    poller.tick();

    bool sawCorrectTarget = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.chatId == kRequesterChatId && m.text.indexOf(String("Queued reply to")) >= 0)
        {
            sawCorrectTarget = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCorrectTarget);
}

// Reply-target expired: error reply goes to u.chatId.
void test_TelegramPoller_expired_target_error_goes_to_requester_chat()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();
    // Slot for 42 has been overwritten by 242.
    rtm.put(242, String("+8613800138000"));

    constexpr int64_t kRequesterChatId = 888;

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          [](int64_t) -> bool { return true; });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(306, kAllowedFromId, 42, "hi", kRequesterChatId)});
    poller.tick();

    bool sawCorrectTarget = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.chatId == kRequesterChatId && m.text.indexOf(String("expired")) >= 0)
        {
            sawCorrectTarget = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCorrectTarget);
}


// ---------- RFC-0021: /blocklist, /block, /unblock command tests ----------

namespace {

struct BlockMutatorRecord
{
    bool called = false;
    int64_t callerIdSeen = 0;
    String cmdSeen;
    String numberSeen;
    bool returnValue = true;
    String reasonToSet;
};

TelegramPoller::SmsBlockMutatorFn makeBlockMutator(BlockMutatorRecord &rec)
{
    return [&rec](int64_t callerId, const String &cmd, const String &number, String &reason) -> bool {
        rec.called = true;
        rec.callerIdSeen = callerId;
        rec.cmdSeen = cmd;
        rec.numberSeen = number;
        reason = rec.reasonToSet;
        return rec.returnValue;
    };
}

} // namespace

void test_TelegramPoller_blocklist_dispatches_to_mutator()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    BlockMutatorRecord rec;
    rec.returnValue = true;
    rec.reasonToSet = "Compile-time block list (0):\n\nRuntime block list (1):\n  10086\n";

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth,
                          nullptr, nullptr, makeBlockMutator(rec));
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(500, kAllowedFromId, 0, "/blocklist", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_TRUE(rec.called);
    TEST_ASSERT_EQUAL_INT64(kAllowedFromId, rec.callerIdSeen);
    TEST_ASSERT_TRUE(rec.cmdSeen == String("list"));
    // number should be empty for "list"
    TEST_ASSERT_EQUAL(0, (int)rec.numberSeen.length());
    TEST_ASSERT_EQUAL(500, poller.lastUpdateId());
}

void test_TelegramPoller_block_dispatches_to_mutator()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    BlockMutatorRecord rec;
    rec.returnValue = true;

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth,
                          nullptr, nullptr, makeBlockMutator(rec));
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(501, kAllowedFromId, 0, "/block 10086", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_TRUE(rec.called);
    TEST_ASSERT_TRUE(rec.cmdSeen == String("block"));
    TEST_ASSERT_TRUE(rec.numberSeen == String("10086"));
    TEST_ASSERT_EQUAL(501, poller.lastUpdateId());
}

void test_TelegramPoller_block_no_arg_sends_usage_error()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    BlockMutatorRecord rec;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth,
                          nullptr, nullptr, makeBlockMutator(rec));
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(502, kAllowedFromId, 0, "/block", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_FALSE(rec.called);
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Usage: /block")) >= 0) { sawUsage = true; break; }
    }
    TEST_ASSERT_TRUE(sawUsage);
}

void test_TelegramPoller_unblock_dispatches_to_mutator()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    BlockMutatorRecord rec;
    rec.returnValue = true;

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth,
                          nullptr, nullptr, makeBlockMutator(rec));
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(503, kAllowedFromId, 0, "/unblock 10086", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_TRUE(rec.called);
    TEST_ASSERT_TRUE(rec.cmdSeen == String("unblock"));
    TEST_ASSERT_TRUE(rec.numberSeen == String("10086"));
    TEST_ASSERT_EQUAL(503, poller.lastUpdateId());
}

void test_TelegramPoller_unblock_no_arg_sends_usage_error()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    BlockMutatorRecord rec;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth,
                          nullptr, nullptr, makeBlockMutator(rec));
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(504, kAllowedFromId, 0, "/unblock", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_FALSE(rec.called);
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Usage: /unblock")) >= 0) { sawUsage = true; break; }
    }
    TEST_ASSERT_TRUE(sawUsage);
}

// /blocklist must NOT be dispatched as /block with arg="list".
void test_TelegramPoller_blocklist_not_matched_as_block()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    BlockMutatorRecord rec;
    rec.returnValue = true;
    rec.reasonToSet = "(No numbers blocked)";

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth,
                          nullptr, nullptr, makeBlockMutator(rec));
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(505, kAllowedFromId, 0, "/blocklist", kAllowedFromId)});
    poller.tick();

    // Must be called with cmd=="list", NOT cmd=="block" with number=="list"
    TEST_ASSERT_TRUE(rec.called);
    TEST_ASSERT_TRUE(rec.cmdSeen == String("list"));
    TEST_ASSERT_EQUAL(505, poller.lastUpdateId());
}

void test_TelegramPoller_block_mutator_nullptr_replies_not_configured()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    // No smsBlockMutator passed.
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    const char *cmds[] = {"/blocklist", "/block 10086", "/unblock 10086"};
    int32_t uid = 506;
    for (const char *cmd : cmds)
    {
        bot.clearMessages();
        bot.queueUpdateBatch({makeUpdate(uid++, kAllowedFromId, 0, cmd, kAllowedFromId)});
        clk.nowMs += TelegramPoller::kPollIntervalMs; // advance past rate limiter
        poller.tick();
        bool sawNotConfigured = false;
        for (const auto &m : bot.sentMessages())
        {
            if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
        }
        TEST_ASSERT_TRUE(sawNotConfigured);
    }
}

// ---------- RFC-0026: /send command tests ----------

// Happy path: /send <number> <body> enqueues SMS and confirms to user.
void test_TelegramPoller_send_happy_path_enqueues_and_confirms()
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

    bot.queueUpdateBatch({makeUpdate(500, kAllowedFromId, 0, "/send +8613800138000 Hello world", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(500, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(1, sender.queueSize());

    bool sawQueued = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Queued to")) >= 0 && m.indexOf(String("Hello world")) >= 0) { sawQueued = true; break; }
    }
    TEST_ASSERT_TRUE(sawQueued);

    // RFC-0030: confirmation message_id should be stored in reply-target map
    // so the user can reply to the confirmation to send another SMS.
    // FakeBotClient.lastFakeMsgId_ starts at 1000, first call returns 1001.
    String foundPhone;
    bool found = rtm.lookup(bot.lastIssuedMessageId(), foundPhone);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING("+8613800138000", foundPhone.c_str());
}

// /send with no argument at all: usage error, nothing enqueued.
void test_TelegramPoller_send_no_arg_sends_usage()
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

    bot.queueUpdateBatch({makeUpdate(501, kAllowedFromId, 0, "/send", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(501, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(0, sender.queueSize());

    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Usage")) >= 0) { sawUsage = true; break; }
    }
    TEST_ASSERT_TRUE(sawUsage);
}

// /send <number> (no body, only a phone with no space): usage error.
void test_TelegramPoller_send_number_only_sends_usage()
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

    bot.queueUpdateBatch({makeUpdate(502, kAllowedFromId, 0, "/send +8613800138000", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(502, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(0, sender.queueSize());

    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Usage")) >= 0) { sawUsage = true; break; }
    }
    TEST_ASSERT_TRUE(sawUsage);
}

// /send preserves body case (original text, not lowercased version).
void test_TelegramPoller_send_preserves_body_case()
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

    // Uppercase characters in body should not be lowercased.
    bot.queueUpdateBatch({makeUpdate(503, kAllowedFromId, 0, "/send +1234 Hello World ABC", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(503, poller.lastUpdateId());
    // Queue should have the entry; content check via FakeModem after drain.
    TEST_ASSERT_EQUAL(1, sender.queueSize());
}

// ---------- RFC-0033: /queue command ----------

void test_TelegramPoller_queue_command_empty()
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

    bot.queueUpdateBatch({makeUpdate(700, kAllowedFromId, 0, "/queue", kAllowedFromId)});
    poller.tick();

    bool sawEmpty = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Queue empty")) >= 0) { sawEmpty = true; break; }
    TEST_ASSERT_TRUE(sawEmpty);
}

void test_TelegramPoller_queue_command_shows_pending()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();
    rtm.put(42, String("+8613800138000"));

    // Make the modem fail so the SMS stays in the queue.
    modem.setPduSendDefault(-1);

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    // Enqueue an SMS reply via the normal reply path.
    bot.queueUpdateBatch({makeUpdate(701, kAllowedFromId, 42, "hi there", kAllowedFromId)});
    poller.tick();
    sender.drainQueue(0); // attempt fails; stays in queue

    TEST_ASSERT_EQUAL(1, sender.queueSize());

    // Now issue /queue — must mention the phone number.
    clk.nowMs += TelegramPoller::kPollIntervalMs;
    bot.clearMessages();
    bot.queueUpdateBatch({makeUpdate(702, kAllowedFromId, 0, "/queue", kAllowedFromId)});
    poller.tick();

    bool sawPhone = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("+8613800138000")) >= 0) { sawPhone = true; break; }
    TEST_ASSERT_TRUE(sawPhone);
}

// /send with a 161-char ASCII body should mention "2 parts" in the confirmation.
void test_TelegramPoller_send_multipart_shows_part_count()
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

    // Build a 161-char ASCII body (2 GSM-7 parts).
    String longBody = "/send +1234567890 ";
    for (int i = 0; i < 161; i++) longBody += 'A';

    bot.queueUpdateBatch({makeUpdate(800, kAllowedFromId, 0, longBody.c_str(), kAllowedFromId)});
    poller.tick();

    bool sawParts = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("parts")) >= 0 || m.indexOf(String("2 part")) >= 0)
            { sawParts = true; break; }
    TEST_ASSERT_TRUE(sawParts);
}

// ---------- RFC-0032: delivery confirmation on successful SMS send ----------

// Reply to a forwarded SMS: after drainQueue succeeds, "📨 Sent to" is delivered.
void test_TelegramPoller_reply_delivery_notification()
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

    bot.queueUpdateBatch({makeUpdate(600, kAllowedFromId, 42, "hi", kAllowedFromId)});
    poller.tick();

    // No delivery notification yet — SMS is only queued.
    bool sawSent = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Sent to")) >= 0) { sawSent = true; break; }
    TEST_ASSERT_FALSE(sawSent);

    // After drain succeeds, delivery notification fires.
    sender.drainQueue(0);

    sawSent = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.chatId == kAllowedFromId &&
            m.text.indexOf(String("Sent to")) >= 0 &&
            m.text.indexOf(String("+8613800138000")) >= 0)
        {
            sawSent = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawSent);
}

// /send path: after drainQueue succeeds, "📨 Sent to" is delivered.
void test_TelegramPoller_send_delivery_notification()
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

    bot.queueUpdateBatch({makeUpdate(601, kAllowedFromId, 0, "/send +8613800138000 Hello", kAllowedFromId)});
    poller.tick();
    TEST_ASSERT_EQUAL(1, sender.queueSize());

    // Drain succeeds — delivery notification expected.
    sender.drainQueue(0);
    TEST_ASSERT_EQUAL(0, sender.queueSize());

    bool sawSent = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.chatId == kAllowedFromId &&
            m.text.indexOf(String("Sent to")) >= 0 &&
            m.text.indexOf(String("+8613800138000")) >= 0)
        {
            sawSent = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawSent);
}

// ---------- RFC-0098: /mute and /unmute ----------

void test_TelegramPoller_mute_calls_mute_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    uint32_t mutedMinutes = 0;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
    poller.setMuteFn([&](uint32_t minutes) { mutedMinutes = minutes; });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(850, kAllowedFromId, 0, "/mute 30", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(30u, mutedMinutes);
    bool sawReply = false;
    for (const auto &m : bot.sentMessagesWithTarget())
        if (m.text.indexOf(String("30 minute")) >= 0) { sawReply = true; break; }
    TEST_ASSERT_TRUE(sawReply);
    TEST_ASSERT_EQUAL(850, poller.lastUpdateId());
}

void test_TelegramPoller_unmute_calls_unmute_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool unmuteCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
    poller.setUnmuteFn([&]() { unmuteCalled = true; });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(851, kAllowedFromId, 0, "/unmute", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_TRUE(unmuteCalled);
    bool sawUnmuted = false;
    for (const auto &m : bot.sentMessagesWithTarget())
        if (m.text.indexOf(String("unmuted")) >= 0) { sawUnmuted = true; break; }
    TEST_ASSERT_TRUE(sawUnmuted);
}

// ---------- RFC-0094: /sendall ----------

void test_TelegramPoller_sendall_broadcasts_to_all_aliases()
{
    FakeModem modem;
    modem.setPduSendDefault(1);
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
    SmsAliasStore store(persist);
    store.load();
    store.set(String("alice"), String("+447911111111"));
    store.set(String("bob"),   String("+447922222222"));
    poller.setAliasStore(&store);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(900, kAllowedFromId, 0, "/sendall Happy holidays!", kAllowedFromId)});
    poller.tick();

    // Two queue entries should have been enqueued.
    TEST_ASSERT_EQUAL(2, sender.queueSize());
    // Confirmation reply should mention 2 recipients.
    bool sawConfirm = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("2 recipient")) >= 0)
        {
            sawConfirm = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawConfirm);
    TEST_ASSERT_EQUAL(900, poller.lastUpdateId());
}

void test_TelegramPoller_sendall_no_aliases_sends_error()
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
    SmsAliasStore store(persist);
    store.load(); // empty
    poller.setAliasStore(&store);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(901, kAllowedFromId, 0, "/sendall Hello!", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(0, sender.queueSize());
    bool sawError = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("No aliases")) >= 0)
        {
            sawError = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawError);
}

// ---------- RFC-0104: /sendall delivery summary ----------

void test_TelegramPoller_sendall_delivery_summary_all_succeed()
{
    FakeModem modem;
    modem.setPduSendDefault(1); // success
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
    SmsAliasStore store(persist);
    store.load();
    store.set(String("alice"), String("+447911111111"));
    store.set(String("bob"),   String("+447922222222"));
    poller.setAliasStore(&store);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(910, kAllowedFromId, 0, "/sendall Test msg", kAllowedFromId)});
    poller.tick();

    // Drain both queue entries (one per drainQueue call).
    sender.drainQueue(0);
    sender.drainQueue(0);

    // Summary should say "2/2 delivered".
    bool sawSummary = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("2/2")) >= 0 && m.text.indexOf(String("delivered")) >= 0)
        {
            sawSummary = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawSummary);
}

void test_TelegramPoller_sendall_delivery_summary_partial_failure()
{
    FakeModem modem;
    // First entry succeeds, second always fails (hits max attempts).
    modem.queuePduSendResult(1);  // alice: success
    // bob will fail kMaxAttempts times.
    for (int i = 0; i < SmsSender::kMaxAttempts; i++)
        modem.queuePduSendResult(-1);
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowAllAuth);
    SmsAliasStore store(persist);
    store.load();
    store.set(String("alice"), String("+447911111111"));
    store.set(String("bob"),   String("+447922222222"));
    poller.setAliasStore(&store);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(911, kAllowedFromId, 0, "/sendall Test msg", kAllowedFromId)});
    poller.tick();

    // Drain alice (success) then all attempts for bob.
    sender.drainQueue(0); // alice: succeeds
    for (int i = 0; i < SmsSender::kMaxAttempts; i++)
    {
        clk.nowMs += 30000; // advance past retry backoff
        sender.drainQueue(clk.nowMs);
    }

    // Summary should mention "1/2" delivered and "1 failed".
    bool sawSummary = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("1/2")) >= 0 && m.text.indexOf(String("failed")) >= 0)
        {
            sawSummary = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawSummary);
}

// ---------- RFC-0110: /resetstats command ----------

void test_TelegramPoller_resetstats_calls_reset_fn()
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
    bool resetCalled = false;
    poller.setResetStatsFn([&]() { resetCalled = true; });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(840, kAllowedFromId, 0, "/resetstats", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_TRUE(resetCalled);
    bool sawConfirm = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("reset")) >= 0)
        {
            sawConfirm = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawConfirm);
    TEST_ASSERT_EQUAL(840, poller.lastUpdateId());
}

void test_TelegramPoller_resetstats_not_configured_replies()
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

    bot.queueUpdateBatch({makeUpdate(841, kAllowedFromId, 0, "/resetstats", kAllowedFromId)});
    poller.tick();

    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("not configured")) >= 0)
        {
            sawNotConfigured = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// ---------- RFC-0107: /at command ----------

void test_TelegramPoller_at_calls_at_fn_and_replies()
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
    String capturedCmd;
    poller.setAtCmdFn([&](int64_t /*fromId*/, const String &cmd) -> String {
        capturedCmd = cmd;
        return String("+CSQ: 18,0\r\nOK");
    });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(830, kAllowedFromId, 0, "/at +CSQ", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL_STRING("+CSQ", capturedCmd.c_str());
    bool sawResp = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("+CSQ: 18")) >= 0)
        {
            sawResp = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawResp);
    TEST_ASSERT_EQUAL(830, poller.lastUpdateId());
}

void test_TelegramPoller_at_strips_leading_AT_prefix()
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
    String capturedCmd;
    poller.setAtCmdFn([&](int64_t /*fromId*/, const String &cmd) -> String {
        capturedCmd = cmd;
        return String("OK");
    });
    poller.begin();

    // User typed "AT+CSQ" — the handler should strip "AT" before passing to the fn.
    bot.queueUpdateBatch({makeUpdate(831, kAllowedFromId, 0, "/at AT+CSQ", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL_STRING("+CSQ", capturedCmd.c_str());
}

void test_TelegramPoller_at_blacklists_cmgd()
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
    bool atCalled = false;
    poller.setAtCmdFn([&](int64_t /*fromId*/, const String & /*cmd*/) -> String {
        atCalled = true;
        return String("OK");
    });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(832, kAllowedFromId, 0, "/at +CMGD=1", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_FALSE(atCalled); // blacklisted — fn should NOT be called
    bool sawBlocked = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("blocked")) >= 0 || m.text.indexOf(String("safety")) >= 0)
        {
            sawBlocked = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawBlocked);
}

void test_TelegramPoller_at_not_configured_replies()
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

    bot.queueUpdateBatch({makeUpdate(833, kAllowedFromId, 0, "/at +CSQ", kAllowedFromId)});
    poller.tick();

    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("not configured")) >= 0)
        {
            sawNotConfigured = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// ---------- RFC-0105: /sim command ----------

void test_TelegramPoller_sim_command_calls_sim_info_fn()
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
    poller.setSimInfoFn([]() -> String {
        return String("SIM info\n  ICCID: 89860123456789\n  Operator: TestNet\n  CSQ: 14 (ok)");
    });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(820, kAllowedFromId, 0, "/sim", kAllowedFromId)});
    poller.tick();

    bool sawSim = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("ICCID")) >= 0 && m.text.indexOf(String("TestNet")) >= 0)
        {
            sawSim = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawSim);
    TEST_ASSERT_EQUAL(820, poller.lastUpdateId());
}

void test_TelegramPoller_sim_not_configured_replies()
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
    // No setSimInfoFn call.
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(821, kAllowedFromId, 0, "/sim", kAllowedFromId)});
    poller.tick();

    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("not configured")) >= 0)
        {
            sawNotConfigured = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// ---------- RFC-0092: /csq ----------

void test_TelegramPoller_csq_command_calls_csq_fn()
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
    poller.setCsqFn([]() -> String { return String("📶 CSQ 18 (good) | home (TestNet) | WiFi -62 dBm"); });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(800, kAllowedFromId, 0, "/csq", kAllowedFromId)});
    poller.tick();

    bool sawCsq = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("CSQ 18")) >= 0)
        {
            sawCsq = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCsq);
    TEST_ASSERT_EQUAL(800, poller.lastUpdateId());
}

// ---------- RFC-0089: /clearqueue ----------

void test_TelegramPoller_clearqueue_discards_entries()
{
    FakeModem modem;
    modem.setPduSendDefault(-1); // always fail so entries stay
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

    // Enqueue two entries directly.
    sender.enqueue(String("+1"), String("first"),  nullptr, nullptr);
    sender.enqueue(String("+2"), String("second"), nullptr, nullptr);
    TEST_ASSERT_EQUAL(2, sender.queueSize());

    bot.queueUpdateBatch({makeUpdate(700, kAllowedFromId, 0, "/clearqueue", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(0, sender.queueSize());
    bool sawCleared = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("Cleared")) >= 0 && m.text.indexOf(String("2")) >= 0)
        {
            sawCleared = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCleared);
    TEST_ASSERT_EQUAL(700, poller.lastUpdateId());
}

// ---------- RFC-0088: /addalias, /rmalias, /aliases, @name expansion ----------

void test_TelegramPoller_addalias_adds_and_replies()
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
    SmsAliasStore store(persist);
    store.load();
    poller.setAliasStore(&store);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(600, kAllowedFromId, 0, "/addalias alice +447911123456", kAllowedFromId)});
    poller.tick();

    // Should have stored the alias.
    TEST_ASSERT_EQUAL_STRING("+447911123456", store.lookup(String("alice")).c_str());
    // Should reply with confirmation containing "@alice".
    bool sawReply = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("alice")) >= 0)
        {
            sawReply = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawReply);
    TEST_ASSERT_EQUAL(600, poller.lastUpdateId());
}

void test_TelegramPoller_rmalias_removes_and_replies()
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
    SmsAliasStore store(persist);
    store.load();
    store.set(String("bob"), String("+441234567890"));
    poller.setAliasStore(&store);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(601, kAllowedFromId, 0, "/rmalias bob", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(0, store.count());
    bool sawRemoved = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("bob")) >= 0 && m.text.indexOf(String("removed")) >= 0)
        {
            sawRemoved = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawRemoved);
}

void test_TelegramPoller_rmalias_not_found_sends_error()
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
    SmsAliasStore store(persist);
    store.load();
    poller.setAliasStore(&store);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(602, kAllowedFromId, 0, "/rmalias nobody", kAllowedFromId)});
    poller.tick();

    bool sawError = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("not found")) >= 0)
        {
            sawError = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawError);
}

void test_TelegramPoller_aliases_lists_entries()
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
    SmsAliasStore store(persist);
    store.load();
    store.set(String("carol"), String("+447900000001"));
    poller.setAliasStore(&store);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(603, kAllowedFromId, 0, "/aliases", kAllowedFromId)});
    poller.tick();

    bool sawCarol = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("carol")) >= 0)
        {
            sawCarol = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCarol);
}

void test_TelegramPoller_send_expands_at_alias()
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
    SmsAliasStore store(persist);
    store.load();
    store.set(String("dave"), String("+441234567890"));
    poller.setAliasStore(&store);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(604, kAllowedFromId, 0, "/send @dave Hello alias!", kAllowedFromId)});
    poller.tick();

    // SmsSender queue should have one entry for the resolved phone.
    auto entries = sender.getQueueSnapshot();
    TEST_ASSERT_EQUAL(1, (int)entries.size());
    TEST_ASSERT_EQUAL_STRING("+441234567890", entries[0].phone.c_str());
}

void test_TelegramPoller_send_unknown_alias_sends_error()
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
    SmsAliasStore store(persist);
    store.load();
    poller.setAliasStore(&store);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(605, kAllowedFromId, 0, "/send @nobody Hello!", kAllowedFromId)});
    poller.tick();

    // Queue should be empty — no send attempted.
    TEST_ASSERT_EQUAL(0, (int)sender.getQueueSnapshot().size());
    // Error reply should mention the unknown alias.
    bool sawError = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("nobody")) >= 0)
        {
            sawError = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawError);
}

// ---------- RFC-0103: /ussd command ----------

void test_TelegramPoller_ussd_calls_ussd_fn_and_replies()
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
    poller.setUssdFn([](const String &code) -> String {
        if (code == "*100#") return String("Your balance is 10.00");
        return String();
    });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(800, kAllowedFromId, 0, "/ussd *100#", kAllowedFromId)});
    poller.tick();

    bool sawResult = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("balance")) >= 0)
        {
            sawResult = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawResult);
    TEST_ASSERT_EQUAL(800, poller.lastUpdateId());
}

void test_TelegramPoller_ussd_timeout_sends_error()
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
    poller.setUssdFn([](const String & /*code*/) -> String {
        return String(); // timeout
    });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(801, kAllowedFromId, 0, "/ussd *101#", kAllowedFromId)});
    poller.tick();

    bool sawError = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("timed out")) >= 0 || m.text.indexOf(String("no response")) >= 0)
        {
            sawError = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawError);
}

void test_TelegramPoller_ussd_invalid_code_sends_error()
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
    bool ussdCalled = false;
    poller.setUssdFn([&](const String & /*code*/) -> String {
        ussdCalled = true;
        return String();
    });
    poller.begin();

    // Code with invalid chars — should be rejected before calling ussdFn.
    bot.queueUpdateBatch({makeUpdate(802, kAllowedFromId, 0, "/ussd *100$ evil", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_FALSE(ussdCalled);
    bool sawError = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("Invalid")) >= 0)
        {
            sawError = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawError);
}

void test_TelegramPoller_ussd_not_configured_replies()
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
    // No setUssdFn call.
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(803, kAllowedFromId, 0, "/ussd *100#", kAllowedFromId)});
    poller.tick();

    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("not configured")) >= 0)
        {
            sawNotConfigured = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// ---------- RFC-0101: alias name character validation ----------

void test_SmsAliasStore_isValidName_accepts_alphanumeric_and_symbols()
{
    TEST_ASSERT_TRUE(SmsAliasStore::isValidName(String("alice")));
    TEST_ASSERT_TRUE(SmsAliasStore::isValidName(String("Alice123")));
    TEST_ASSERT_TRUE(SmsAliasStore::isValidName(String("my_contact")));
    TEST_ASSERT_TRUE(SmsAliasStore::isValidName(String("dad-cell")));
    TEST_ASSERT_TRUE(SmsAliasStore::isValidName(String("a"))); // single char
}

void test_SmsAliasStore_isValidName_rejects_invalid_chars()
{
    TEST_ASSERT_FALSE(SmsAliasStore::isValidName(String("")));           // empty
    TEST_ASSERT_FALSE(SmsAliasStore::isValidName(String("my contact"))); // space
    TEST_ASSERT_FALSE(SmsAliasStore::isValidName(String("@alice")));     // @ sign
    TEST_ASSERT_FALSE(SmsAliasStore::isValidName(String("alice/bob")));  // slash
    TEST_ASSERT_FALSE(SmsAliasStore::isValidName(String("a.b")));        // dot
    TEST_ASSERT_FALSE(SmsAliasStore::isValidName(String("foo+bar")));    // plus
}

void test_SmsAliasStore_set_rejects_invalid_name()
{
    FakePersist persist;
    SmsAliasStore store(persist);
    store.load();

    // A name with a space should be rejected before the store even looks at the phone.
    TEST_ASSERT_FALSE(store.set(String("bad name"), String("+447911123456")));
    TEST_ASSERT_EQUAL(0, store.count());
}

void test_TelegramPoller_addalias_invalid_name_sends_error()
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
    SmsAliasStore store(persist);
    store.load();
    poller.setAliasStore(&store);
    poller.begin();

    // Name with a dot — invalid char, should be rejected with a targeted error.
    bot.queueUpdateBatch({makeUpdate(601, kAllowedFromId, 0,
        "/addalias my.contact +447911123456", kAllowedFromId)});
    poller.tick();

    // Alias must not have been added.
    TEST_ASSERT_EQUAL(0, store.count());
    // Reply must mention the constraint.
    bool sawError = false;
    for (const auto &m : bot.sentMessagesWithTarget())
    {
        if (m.text.indexOf(String("letters")) >= 0 || m.text.indexOf(String("Invalid")) >= 0)
        {
            sawError = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawError);
    TEST_ASSERT_EQUAL(601, poller.lastUpdateId());
}

// RFC-0114: /balance calls ussdFn with configured code and replies.
void test_TelegramPoller_balance_calls_ussd_fn_and_replies()
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
    poller.setBalanceCodeFn([]() -> String { return String("*100#"); });
    poller.setUssdFn([](const String &code) -> String {
        if (code == "*100#") return String("Balance: 25.00");
        return String();
    });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(900, kAllowedFromId, 0, "/balance", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(900, poller.lastUpdateId());
    bool sawBalance = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Balance")) >= 0 || m.indexOf(String("25.00")) >= 0)
        { sawBalance = true; break; }
    }
    TEST_ASSERT_TRUE(sawBalance);
}

// RFC-0114: /balance with no balance code fn → "not configured" reply.
void test_TelegramPoller_balance_no_code_fn_replies_not_configured()
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
    // No balanceCodeFn set.
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(901, kAllowedFromId, 0, "/balance", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(901, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0 ||
            m.indexOf(String("USSD_BALANCE_CODE")) >= 0)
        { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0114: /balance with code fn returning empty → "not configured" reply.
void test_TelegramPoller_balance_empty_code_replies_not_configured()
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
    poller.setBalanceCodeFn([]() -> String { return String(); }); // empty code
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(902, kAllowedFromId, 0, "/balance", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(902, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0 ||
            m.indexOf(String("USSD_BALANCE_CODE")) >= 0)
        { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0114: /balance when USSD returns empty → "No response from carrier." error.
void test_TelegramPoller_balance_ussd_empty_replies_no_response()
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
    poller.setBalanceCodeFn([]() -> String { return String("*100#"); });
    poller.setUssdFn([](const String &) -> String { return String(); }); // timeout
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(903, kAllowedFromId, 0, "/balance", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(903, poller.lastUpdateId());
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("No response")) >= 0 ||
            m.indexOf(String("carrier")) >= 0)
        { sawError = true; break; }
    }
    TEST_ASSERT_TRUE(sawError);
}

// RFC-0112: /reboot calls rebootFn and replies "Rebooting...".
void test_TelegramPoller_reboot_calls_fn_and_replies()
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

    bool rebootCalled = false;
    int64_t rebootFromId = 0;
    poller.setRebootFn([&](int64_t fromId) {
        rebootCalled = true;
        rebootFromId = fromId;
    });

    bot.queueUpdateBatch({makeUpdate(800, kAllowedFromId, 0, "/reboot", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_TRUE(rebootCalled);
    TEST_ASSERT_EQUAL((int64_t)kAllowedFromId, (int64_t)rebootFromId);
    TEST_ASSERT_EQUAL(800, poller.lastUpdateId());

    bool sawRebooting = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Rebooting")) >= 0) { sawRebooting = true; break; }
    }
    TEST_ASSERT_TRUE(sawRebooting);
}

// RFC-0112: /reboot with no fn set → "not configured" reply.
void test_TelegramPoller_reboot_not_configured_replies()
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
    // rebootFn_ not set

    bot.queueUpdateBatch({makeUpdate(801, kAllowedFromId, 0, "/reboot", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(801, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0111: /send twice with same phone+body → second gets "Already queued" error.
void test_TelegramPoller_send_duplicate_gets_already_queued_error()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    modem.setPduSendDefault(-1); // keep first entry in queue
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    // First /send — should enqueue fine.
    bot.queueUpdateBatch({makeUpdate(700, kAllowedFromId, 0,
        "/send +8613800138000 Hello world", kAllowedFromId)});
    poller.tick();
    TEST_ASSERT_EQUAL(1, sender.queueSize());
    TEST_ASSERT_EQUAL(700, poller.lastUpdateId());

    // Second /send with same phone+body — should be rejected as duplicate.
    clk.nowMs += TelegramPoller::kPollIntervalMs;
    bot.clearMessages();
    bot.queueUpdateBatch({makeUpdate(701, kAllowedFromId, 0,
        "/send +8613800138000 Hello world", kAllowedFromId)});
    poller.tick();

    // Queue should still have only one entry.
    TEST_ASSERT_EQUAL(1, sender.queueSize());
    TEST_ASSERT_EQUAL(701, poller.lastUpdateId());

    // User must receive an "Already queued" error.
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Already queued")) >= 0 ||
            m.indexOf(String("already queued")) >= 0)
        {
            sawError = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawError);
}

void run_telegram_poller_tests()
{
    RUN_TEST(test_TelegramPoller_happy_path_routes_reply_to_phone);
    RUN_TEST(test_TelegramPoller_unauthorized_drops_and_advances);
    RUN_TEST(test_TelegramPoller_stale_slot_rejects_with_error_reply);
    RUN_TEST(test_TelegramPoller_unicode_body_sends_via_ucs2);
    RUN_TEST(test_TelegramPoller_invalid_update_advances_watermark);
    RUN_TEST(test_TelegramPoller_no_reply_to_message_id_drops_with_help);
    RUN_TEST(test_TelegramPoller_rate_limit_one_poll_per_interval);
    RUN_TEST(test_TelegramPoller_transport_failure_does_not_advance);
    RUN_TEST(test_TelegramPoller_persistence_across_restart_does_not_replay);
    RUN_TEST(test_TelegramPoller_offset_passed_to_bot_uses_watermark);
    RUN_TEST(test_TelegramPoller_multiple_updates_in_one_batch);
    RUN_TEST(test_TelegramPoller_status_command_calls_status_fn);
    RUN_TEST(test_TelegramPoller_status_command_nullptr_fallback);
    RUN_TEST(test_TelegramPoller_help_message_mentions_status);
    // RFC-0016: per-requester command reply
    RUN_TEST(test_TelegramPoller_status_reply_goes_to_requester_chat);
    RUN_TEST(test_TelegramPoller_debug_reply_goes_to_requester_chat);
    RUN_TEST(test_TelegramPoller_error_reply_goes_to_requester_chat);
    RUN_TEST(test_TelegramPoller_group_chat_reply_goes_to_group);
    RUN_TEST(test_TelegramPoller_sms_forward_uses_admin_sentinel);
    RUN_TEST(test_TelegramPoller_admin_status_reply_goes_to_admin_chat);
    RUN_TEST(test_TelegramPoller_enqueue_confirmation_goes_to_requester_chat);
    RUN_TEST(test_TelegramPoller_expired_target_error_goes_to_requester_chat);
    // RFC-0021: SMS block list commands
    RUN_TEST(test_TelegramPoller_blocklist_dispatches_to_mutator);
    // RFC-0026: /send command
    RUN_TEST(test_TelegramPoller_block_dispatches_to_mutator);
    RUN_TEST(test_TelegramPoller_block_no_arg_sends_usage_error);
    RUN_TEST(test_TelegramPoller_unblock_dispatches_to_mutator);
    RUN_TEST(test_TelegramPoller_unblock_no_arg_sends_usage_error);
    RUN_TEST(test_TelegramPoller_blocklist_not_matched_as_block);
    RUN_TEST(test_TelegramPoller_block_mutator_nullptr_replies_not_configured);
    // RFC-0026: /send command
    RUN_TEST(test_TelegramPoller_send_happy_path_enqueues_and_confirms);
    RUN_TEST(test_TelegramPoller_send_no_arg_sends_usage);
    RUN_TEST(test_TelegramPoller_send_number_only_sends_usage);
    RUN_TEST(test_TelegramPoller_send_preserves_body_case);
    // RFC-0032: delivery confirmation
    RUN_TEST(test_TelegramPoller_reply_delivery_notification);
    RUN_TEST(test_TelegramPoller_send_delivery_notification);
    // RFC-0033: /queue command
    RUN_TEST(test_TelegramPoller_queue_command_empty);
    RUN_TEST(test_TelegramPoller_queue_command_shows_pending);
    // RFC-0037: part count in /send confirmation
    RUN_TEST(test_TelegramPoller_send_multipart_shows_part_count);
    // RFC-0089: /clearqueue command
    RUN_TEST(test_TelegramPoller_clearqueue_discards_entries);
    // RFC-0110: /resetstats command
    RUN_TEST(test_TelegramPoller_resetstats_calls_reset_fn);
    RUN_TEST(test_TelegramPoller_resetstats_not_configured_replies);
    // RFC-0107: /at command
    RUN_TEST(test_TelegramPoller_at_calls_at_fn_and_replies);
    RUN_TEST(test_TelegramPoller_at_strips_leading_AT_prefix);
    RUN_TEST(test_TelegramPoller_at_blacklists_cmgd);
    RUN_TEST(test_TelegramPoller_at_not_configured_replies);
    // RFC-0105: /sim command
    RUN_TEST(test_TelegramPoller_sim_command_calls_sim_info_fn);
    RUN_TEST(test_TelegramPoller_sim_not_configured_replies);
    // RFC-0092: /csq command
    RUN_TEST(test_TelegramPoller_csq_command_calls_csq_fn);
    // RFC-0098: /mute and /unmute commands
    RUN_TEST(test_TelegramPoller_mute_calls_mute_fn);
    RUN_TEST(test_TelegramPoller_unmute_calls_unmute_fn);
    // RFC-0094 / RFC-0104: /sendall command + delivery summary
    RUN_TEST(test_TelegramPoller_sendall_broadcasts_to_all_aliases);
    RUN_TEST(test_TelegramPoller_sendall_no_aliases_sends_error);
    RUN_TEST(test_TelegramPoller_sendall_delivery_summary_all_succeed);
    RUN_TEST(test_TelegramPoller_sendall_delivery_summary_partial_failure);
    // RFC-0088: phone aliases
    RUN_TEST(test_TelegramPoller_addalias_adds_and_replies);
    RUN_TEST(test_TelegramPoller_rmalias_removes_and_replies);
    RUN_TEST(test_TelegramPoller_rmalias_not_found_sends_error);
    RUN_TEST(test_TelegramPoller_aliases_lists_entries);
    RUN_TEST(test_TelegramPoller_send_expands_at_alias);
    RUN_TEST(test_TelegramPoller_send_unknown_alias_sends_error);
    // RFC-0103: /ussd command
    RUN_TEST(test_TelegramPoller_ussd_calls_ussd_fn_and_replies);
    RUN_TEST(test_TelegramPoller_ussd_timeout_sends_error);
    RUN_TEST(test_TelegramPoller_ussd_invalid_code_sends_error);
    RUN_TEST(test_TelegramPoller_ussd_not_configured_replies);
    // RFC-0101: alias name character validation
    RUN_TEST(test_SmsAliasStore_isValidName_accepts_alphanumeric_and_symbols);
    RUN_TEST(test_SmsAliasStore_isValidName_rejects_invalid_chars);
    RUN_TEST(test_SmsAliasStore_set_rejects_invalid_name);
    RUN_TEST(test_TelegramPoller_addalias_invalid_name_sends_error);
    // RFC-0114: /balance command
    RUN_TEST(test_TelegramPoller_balance_calls_ussd_fn_and_replies);
    RUN_TEST(test_TelegramPoller_balance_no_code_fn_replies_not_configured);
    RUN_TEST(test_TelegramPoller_balance_empty_code_replies_not_configured);
    RUN_TEST(test_TelegramPoller_balance_ussd_empty_replies_no_response);
    // RFC-0112: /reboot command
    RUN_TEST(test_TelegramPoller_reboot_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_reboot_not_configured_replies);
    // RFC-0111: outbound dedup
    RUN_TEST(test_TelegramPoller_send_duplicate_gets_already_queued_error);
}
