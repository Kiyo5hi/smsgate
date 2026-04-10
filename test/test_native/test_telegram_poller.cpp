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
#include "sms_debug_log.h"
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

// RFC-0118: /label replies with current device label.
void test_TelegramPoller_label_replies_with_current_label()
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
    poller.setLabelGetFn([]() -> String { return String("Office SIM"); });
    poller.setLabelSetFn([](const String &) {});
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(820, kAllowedFromId, 0, "/label", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(820, poller.lastUpdateId());
    bool sawLabel = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Office SIM")) >= 0) { sawLabel = true; break; }
    }
    TEST_ASSERT_TRUE(sawLabel);
}

// RFC-0118: /label with no label set replies "(no label set)".
void test_TelegramPoller_label_no_label_set_replies_placeholder()
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
    poller.setLabelGetFn([]() -> String { return String(); }); // empty
    poller.setLabelSetFn([](const String &) {});
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(821, kAllowedFromId, 0, "/label", kAllowedFromId)});
    poller.tick();

    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("no label")) >= 0) { sawPlaceholder = true; break; }
    }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0118: /setlabel <name> calls setter and replies confirmation.
void test_TelegramPoller_setlabel_calls_setter_and_confirms()
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
    String savedLabel;
    poller.setLabelGetFn([]() -> String { return String(); });
    poller.setLabelSetFn([&](const String &lbl) { savedLabel = lbl; });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(822, kAllowedFromId, 0, "/setlabel Site-A", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(822, poller.lastUpdateId());
    TEST_ASSERT_EQUAL_STRING("Site-A", savedLabel.c_str());
    bool sawConfirm = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Site-A")) >= 0) { sawConfirm = true; break; }
    }
    TEST_ASSERT_TRUE(sawConfirm);
}

// RFC-0118: /setlabel with no argument sends usage hint.
void test_TelegramPoller_setlabel_no_arg_sends_usage()
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
    bool setterCalled = false;
    poller.setLabelGetFn([]() -> String { return String(); });
    poller.setLabelSetFn([&](const String &) { setterCalled = true; });
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(823, kAllowedFromId, 0, "/setlabel", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_FALSE(setterCalled);
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Usage")) >= 0 || m.indexOf(String("usage")) >= 0)
        { sawUsage = true; break; }
    }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0118: /setlabel with name > 32 chars sends error.
void test_TelegramPoller_setlabel_too_long_sends_error()
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
    bool setterCalled = false;
    poller.setLabelGetFn([]() -> String { return String(); });
    poller.setLabelSetFn([&](const String &) { setterCalled = true; });
    poller.begin();

    // 33-char name
    bot.queueUpdateBatch({makeUpdate(824, kAllowedFromId, 0,
        "/setlabel ThisLabelIsWayTooLongForOurLimit!", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_FALSE(setterCalled);
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("long")) >= 0 || m.indexOf(String("max")) >= 0)
        { sawError = true; break; }
    }
    TEST_ASSERT_TRUE(sawError);
}

// RFC-0117: /history <filter> replies with filtered log entries.
void test_TelegramPoller_history_filters_debug_log()
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
    SmsDebugLog log;
    // Push two entries: one matching, one not.
    SmsDebugLog::Entry e1;
    e1.sender = String("+8613800138000");
    e1.outcome = String("in:forwarded");
    e1.bodyChars = 10;
    log.push(e1);
    SmsDebugLog::Entry e2;
    e2.sender = String("+447911123456");
    e2.outcome = String("out:sent");
    e2.bodyChars = 5;
    log.push(e2);
    poller.setDebugLog(&log);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(810, kAllowedFromId, 0, "/history +8613", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(810, poller.lastUpdateId());
    bool sawMatch = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("+86138")) >= 0) { sawMatch = true; break; }
    }
    TEST_ASSERT_TRUE(sawMatch);
    // Should NOT contain the UK number.
    for (const auto &m : bot.sentMessages())
    {
        TEST_ASSERT_EQUAL(-1, m.indexOf(String("+44791")));
    }
}

// RFC-0117: /history with no argument sends usage hint.
void test_TelegramPoller_history_no_arg_sends_usage()
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
    SmsDebugLog log;
    poller.setDebugLog(&log);
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(811, kAllowedFromId, 0, "/history", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(811, poller.lastUpdateId());
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Usage")) >= 0 || m.indexOf(String("usage")) >= 0)
        { sawUsage = true; break; }
    }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0117: /history with no debug log set replies "not configured".
void test_TelegramPoller_history_no_log_replies_not_configured()
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
    // No setDebugLog call.
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(812, kAllowedFromId, 0, "/history +8613", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(812, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
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

// RFC-0119: /ping with pingSummaryFn set → replies with fn result.
void test_TelegramPoller_ping_uses_summary_fn()
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
    poller.setPingSummaryFn([]() -> String {
        return String("\xF0\x9F\x8F\x93 Pong [Office] | \xE2\x8F\xB1 1d 2h 15m | CSQ 18 (good)");
    });

    bot.queueUpdateBatch({makeUpdate(900, kAllowedFromId, 0, "/ping", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(900, poller.lastUpdateId());
    bool sawSummary = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Office")) >= 0 && m.indexOf(String("CSQ")) >= 0)
        {
            sawSummary = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawSummary);
}

// RFC-0119: /ping without pingSummaryFn → falls back to plain "Pong".
void test_TelegramPoller_ping_fallback_without_fn()
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
    // pingSummaryFn_ NOT set

    bot.queueUpdateBatch({makeUpdate(901, kAllowedFromId, 0, "/ping", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(901, poller.lastUpdateId());
    bool sawPong = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Pong")) >= 0) { sawPong = true; break; }
    }
    TEST_ASSERT_TRUE(sawPong);
}

// RFC-0120: /uptime with fn set → replies with fn result.
void test_TelegramPoller_uptime_calls_fn_and_replies()
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
    poller.setUptimeFn([]() -> String { return String("\xE2\x8F\xB1 0d 0h 5m 3s"); });

    bot.queueUpdateBatch({makeUpdate(910, kAllowedFromId, 0, "/uptime", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(910, poller.lastUpdateId());
    bool sawUptime = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("5m")) >= 0) { sawUptime = true; break; }
    }
    TEST_ASSERT_TRUE(sawUptime);
}

// RFC-0120: /uptime without fn → "(uptime not configured)".
void test_TelegramPoller_uptime_not_configured_replies()
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
    // uptimeFn_ NOT set

    bot.queueUpdateBatch({makeUpdate(911, kAllowedFromId, 0, "/uptime", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(911, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0121: /network with fn set → replies with fn result.
void test_TelegramPoller_network_calls_fn_and_replies()
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
    poller.setNetworkFn([]() -> String {
        return String("\xF0\x9F\x93\xB6 Operator: T-Mobile | Reg: home | CSQ 18 (good)");
    });

    bot.queueUpdateBatch({makeUpdate(912, kAllowedFromId, 0, "/network", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(912, poller.lastUpdateId());
    bool sawNetwork = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("T-Mobile")) >= 0 && m.indexOf(String("Reg:")) >= 0)
        {
            sawNetwork = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawNetwork);
}

// RFC-0121: /network without fn → "(network info not configured)".
void test_TelegramPoller_network_not_configured_replies()
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
    // networkFn_ NOT set

    bot.queueUpdateBatch({makeUpdate(913, kAllowedFromId, 0, "/network", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(913, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0122: /logs with debug log set → replies with dumpBrief(N).
void test_TelegramPoller_logs_returns_debug_log_entries()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsDebugLog log;
    SmsDebugLog::Entry e1;
    e1.sender    = String("+1234567890");
    e1.bodyChars = 5;
    e1.outcome   = String("fwd");
    log.push(e1);

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(920, kAllowedFromId, 0, "/logs", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(920, poller.lastUpdateId());
    bool sawEntry = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("+1234567890")) >= 0) { sawEntry = true; break; }
    }
    TEST_ASSERT_TRUE(sawEntry);
}

// RFC-0122: /logs 2 respects the N argument.
void test_TelegramPoller_logs_respects_n_arg()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsDebugLog log;
    for (int i = 0; i < 5; i++) {
        SmsDebugLog::Entry e;
        e.sender    = String("+111000000") + String(i);
        e.bodyChars = 3;
        e.outcome   = String("fwd");
        log.push(e);
    }

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(921, kAllowedFromId, 0, "/logs 2", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(921, poller.lastUpdateId());
    // Just verify a reply was sent (content is format-checked in test_sms_debug_log tests)
    TEST_ASSERT_TRUE(bot.sentMessages().size() > 0);
}

// RFC-0122: /logs without debug log → "(debug log not configured)".
void test_TelegramPoller_logs_not_configured_replies()
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
    // debugLog_ NOT set

    bot.queueUpdateBatch({makeUpdate(922, kAllowedFromId, 0, "/logs", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(922, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0123: /boot with fn set → replies with fn result.
void test_TelegramPoller_boot_calls_fn_and_replies()
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
    poller.setBootInfoFn([]() -> String {
        return String("\xF0\x9F\x94\x84 Boot #7 | Reason: Power-on | 2026-04-10 12:00 UTC");
    });

    bot.queueUpdateBatch({makeUpdate(923, kAllowedFromId, 0, "/boot", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(923, poller.lastUpdateId());
    bool sawBoot = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Boot #7")) >= 0) { sawBoot = true; break; }
    }
    TEST_ASSERT_TRUE(sawBoot);
}

// RFC-0123: /boot without fn → "(boot info not configured)".
void test_TelegramPoller_boot_not_configured_replies()
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
    // bootInfoFn_ NOT set

    bot.queueUpdateBatch({makeUpdate(924, kAllowedFromId, 0, "/boot", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(924, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0124: /count with fn set → replies with fn result.
void test_TelegramPoller_count_calls_fn_and_replies()
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
    poller.setCountFn([]() -> String {
        return String("\xF0\x9F\x93\x8A SMS rcvd: 12 | fwd: 11 | fail: 1 | Calls: 3");
    });

    bot.queueUpdateBatch({makeUpdate(930, kAllowedFromId, 0, "/count", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(930, poller.lastUpdateId());
    bool sawCount = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("rcvd")) >= 0 && m.indexOf(String("Calls")) >= 0)
        {
            sawCount = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCount);
}

// RFC-0124: /count without fn → "(count not configured)".
void test_TelegramPoller_count_not_configured_replies()
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
    // countFn_ NOT set

    bot.queueUpdateBatch({makeUpdate(931, kAllowedFromId, 0, "/count", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(931, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0125: /me replies with fromId and chatId.
void test_TelegramPoller_me_replies_with_ids()
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

    bot.queueUpdateBatch({makeUpdate(932, kAllowedFromId, 0, "/me", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(932, poller.lastUpdateId());
    bool sawIds = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("fromId")) >= 0 && m.indexOf(String("chatId")) >= 0)
        {
            sawIds = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawIds);
}

// RFC-0125: /me works even for unauthorized users.
void test_TelegramPoller_me_works_for_unauthorized_user()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    constexpr int64_t kUnauthorizedId = 99999;

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth); // kAllowedFromId != kUnauthorizedId
    poller.begin();

    bot.queueUpdateBatch({makeUpdate(933, kUnauthorizedId, 0, "/me", kUnauthorizedId)});
    poller.tick();

    TEST_ASSERT_EQUAL(933, poller.lastUpdateId());
    bool sawIds = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("fromId")) >= 0) { sawIds = true; break; }
    }
    TEST_ASSERT_TRUE(sawIds);
}

// RFC-0126: /ip with fn set → replies with fn result.
void test_TelegramPoller_ip_calls_fn_and_replies()
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
    poller.setIpFn([]() -> String {
        return String("\xF0\x9F\x8C\x90 192.168.1.42 | SSID: TestNet | RSSI: -65 dBm");
    });

    bot.queueUpdateBatch({makeUpdate(940, kAllowedFromId, 0, "/ip", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(940, poller.lastUpdateId());
    bool sawIp = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("192.168.1.42")) >= 0) { sawIp = true; break; }
    }
    TEST_ASSERT_TRUE(sawIp);
}

// RFC-0126: /ip without fn → "(ip info not configured)".
void test_TelegramPoller_ip_not_configured_replies()
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
    // ipFn_ NOT set

    bot.queueUpdateBatch({makeUpdate(941, kAllowedFromId, 0, "/ip", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(941, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0127: /smsslots with fn set → replies with fn result.
void test_TelegramPoller_smsslots_calls_fn_and_replies()
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
    poller.setSmsSlotssFn([]() -> String {
        return String("\xF0\x9F\x93\xA8 SIM slots: 5/30 used (16%)");
    });

    bot.queueUpdateBatch({makeUpdate(942, kAllowedFromId, 0, "/smsslots", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(942, poller.lastUpdateId());
    bool sawSlots = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("SIM slots")) >= 0) { sawSlots = true; break; }
    }
    TEST_ASSERT_TRUE(sawSlots);
}

// RFC-0127: /smsslots without fn → "(SMS slots info not configured)".
void test_TelegramPoller_smsslots_not_configured_replies()
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
    // smsSlotsFn_ NOT set

    bot.queueUpdateBatch({makeUpdate(943, kAllowedFromId, 0, "/smsslots", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(943, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0128: /lifetime with fn set → replies with fn result.
void test_TelegramPoller_lifetime_calls_fn_and_replies()
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
    poller.setLifetimeFn([]() -> String {
        return String("\xF0\x9F\x93\x88 Lifetime: 1234 SMS forwarded | Boot #7");
    });

    bot.queueUpdateBatch({makeUpdate(950, kAllowedFromId, 0, "/lifetime", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(950, poller.lastUpdateId());
    bool sawLifetime = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("1234")) >= 0) { sawLifetime = true; break; }
    }
    TEST_ASSERT_TRUE(sawLifetime);
}

// RFC-0128: /lifetime without fn → "(lifetime stats not configured)".
void test_TelegramPoller_lifetime_not_configured_replies()
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
    // lifetimeFn_ NOT set

    bot.queueUpdateBatch({makeUpdate(951, kAllowedFromId, 0, "/lifetime", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(951, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0129: /announce with fn set → calls fn and replies with count.
void test_TelegramPoller_announce_calls_fn_and_replies()
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

    int announceCount = 0;
    String capturedMsg;
    poller.setAnnounceFn([&](const String &msg) -> int {
        capturedMsg = msg;
        announceCount++;
        return 2; // simulates 2 users
    });

    bot.queueUpdateBatch({makeUpdate(952, kAllowedFromId, 0, "/announce Hello all!", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(952, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(1, announceCount);
    TEST_ASSERT_EQUAL_STRING("Hello all!", capturedMsg.c_str());
    bool sawConfirm = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Announced")) >= 0 && m.indexOf(String("2")) >= 0)
        {
            sawConfirm = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawConfirm);
}

// RFC-0129: /announce with no argument → usage error.
void test_TelegramPoller_announce_no_arg_sends_usage()
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
    poller.setAnnounceFn([](const String &) -> int { return 0; });

    bot.queueUpdateBatch({makeUpdate(953, kAllowedFromId, 0, "/announce", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(953, poller.lastUpdateId());
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Usage")) >= 0) { sawUsage = true; break; }
    }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0129: /announce without fn → "(announce not configured)".
void test_TelegramPoller_announce_not_configured_replies()
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
    // announceFn_ NOT set

    bot.queueUpdateBatch({makeUpdate(954, kAllowedFromId, 0, "/announce test", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(954, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0130: /digest with fn set → replies with fn result.
void test_TelegramPoller_digest_calls_fn_and_replies()
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
    poller.setDigestFn([]() -> String {
        return String("\xF0\x9F\x93\x8A On-demand digest | fwd 5 (session) 42 (lifetime)");
    });

    bot.queueUpdateBatch({makeUpdate(960, kAllowedFromId, 0, "/digest", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(960, poller.lastUpdateId());
    bool sawDigest = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("digest")) >= 0 && m.indexOf(String("fwd")) >= 0)
        {
            sawDigest = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawDigest);
}

// RFC-0130: /digest without fn → "(digest not configured)".
void test_TelegramPoller_digest_not_configured_replies()
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

    bot.queueUpdateBatch({makeUpdate(961, kAllowedFromId, 0, "/digest", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(961, poller.lastUpdateId());
    bool sawNotConfigured = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("not configured")) >= 0) { sawNotConfigured = true; break; }
    }
    TEST_ASSERT_TRUE(sawNotConfigured);
}

// RFC-0131: /note with fn set → replies with current note.
void test_TelegramPoller_note_replies_with_note()
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
    poller.setNoteGetFn([]() -> String { return String("SIM changed 2026-04-10"); });

    bot.queueUpdateBatch({makeUpdate(962, kAllowedFromId, 0, "/note", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(962, poller.lastUpdateId());
    bool sawNote = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("SIM changed")) >= 0) { sawNote = true; break; }
    }
    TEST_ASSERT_TRUE(sawNote);
}

// RFC-0131: /note with empty note → "(no note set)".
void test_TelegramPoller_note_empty_replies_placeholder()
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
    poller.setNoteGetFn([]() -> String { return String(""); });

    bot.queueUpdateBatch({makeUpdate(963, kAllowedFromId, 0, "/note", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(963, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("no note set")) >= 0) { sawPlaceholder = true; break; }
    }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0131: /setnote <text> calls setter and confirms.
void test_TelegramPoller_setnote_calls_setter_and_confirms()
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
    String savedNote;
    poller.setNoteGetFn([&]() -> String { return savedNote; });
    poller.setNoteSetFn([&](const String &n) { savedNote = n; });

    bot.queueUpdateBatch({makeUpdate(964, kAllowedFromId, 0, "/setnote Office SIM", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(964, poller.lastUpdateId());
    TEST_ASSERT_EQUAL_STRING("Office SIM", savedNote.c_str());
    bool sawConfirm = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("saved")) >= 0) { sawConfirm = true; break; }
    }
    TEST_ASSERT_TRUE(sawConfirm);
}

// RFC-0132: /exportaliases with aliases → replies with name=number lines.
void test_TelegramPoller_exportaliases_replies_with_csv()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsAliasStore store(persist);
    store.set(String("alice"), String("+447911123456"));
    store.set(String("bob"),   String("+447911654321"));

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setAliasStore(&store);

    bot.queueUpdateBatch({makeUpdate(970, kAllowedFromId, 0, "/exportaliases", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(970, poller.lastUpdateId());
    bool sawAlice = false;
    bool sawBob   = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("alice=+447911123456")) >= 0) sawAlice = true;
        if (m.indexOf(String("bob=+447911654321")) >= 0)   sawBob   = true;
    }
    TEST_ASSERT_TRUE(sawAlice);
    TEST_ASSERT_TRUE(sawBob);
}

// RFC-0132: /exportaliases with empty store → "(no aliases)".
void test_TelegramPoller_exportaliases_empty_store_replies_placeholder()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsAliasStore store(persist);

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setAliasStore(&store);

    bot.queueUpdateBatch({makeUpdate(971, kAllowedFromId, 0, "/exportaliases", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(971, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("no aliases")) >= 0) { sawPlaceholder = true; break; }
    }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0133: /shortcuts replies with a quick reference containing key commands.
void test_TelegramPoller_shortcuts_replies_with_quick_ref()
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

    bot.queueUpdateBatch({makeUpdate(972, kAllowedFromId, 0, "/shortcuts", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(972, poller.lastUpdateId());
    bool sawPing  = false;
    bool sawSend  = false;
    bool sawHelp  = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("/ping")) >= 0) sawPing = true;
        if (m.indexOf(String("/send")) >= 0) sawSend = true;
        if (m.indexOf(String("/help")) >= 0) sawHelp = true;
    }
    TEST_ASSERT_TRUE(sawPing);
    TEST_ASSERT_TRUE(sawSend);
    TEST_ASSERT_TRUE(sawHelp);
}

// RFC-0136: /cancelnum with matching entries → removes and confirms.
void test_TelegramPoller_cancelnum_removes_matching_entries()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    modem.setPduSendDefault(-1); // keep entries in queue
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();

    // Enqueue two SMS to the same number and one to a different number.
    sender.enqueue(String("+447911111111"), String("msg1"), nullptr);
    sender.enqueue(String("+447911111111"), String("msg2"), nullptr);
    sender.enqueue(String("+447922222222"), String("msg3"), nullptr);

    bot.queueUpdateBatch({makeUpdate(990, kAllowedFromId, 0, "/cancelnum +447911111111", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(990, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(1, sender.queueSize()); // only the +447922222222 entry remains
    bool sawCancelled = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Cancelled")) >= 0 && m.indexOf(String("2")) >= 0)
        {
            sawCancelled = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCancelled);
}

// RFC-0136: /cancelnum with no matching entries → placeholder.
void test_TelegramPoller_cancelnum_no_match_replies_placeholder()
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

    bot.queueUpdateBatch({makeUpdate(991, kAllowedFromId, 0, "/cancelnum +447911111111", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(991, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("no queued entries")) >= 0) { sawPlaceholder = true; break; }
    }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0136: /cancelnum with no arg → usage error.
void test_TelegramPoller_cancelnum_no_arg_sends_usage()
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

    bot.queueUpdateBatch({makeUpdate(992, kAllowedFromId, 0, "/cancelnum", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(992, poller.lastUpdateId());
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Usage")) >= 0) { sawUsage = true; break; }
    }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0188: /schedulesend 1 +1234 Hello → slot occupied, fires after delay.
void test_TelegramPoller_schedulesend_fires_after_delay()
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
                          allowedAuth);
    poller.begin();
    poller.tick(); // consume initial poll
    bot.clearMessages();

    bot.queueUpdateBatch({makeUpdate(993, kAllowedFromId, 0,
        "/schedulesend 1 +13800138000 Delayed message", kAllowedFromId)});
    clk.nowMs += 4000; // advance past poll interval
    poller.tick();

    // Should have confirmed scheduling
    bool sawScheduled = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("scheduled") >= 0) { sawScheduled = true; break; }
    TEST_ASSERT_TRUE(sawScheduled);

    // Advance past the 1-minute delay (60000ms)
    bot.clearMessages();
    clk.nowMs += 60000UL;
    // tick() drains scheduled queue — this should enqueue the SMS
    poller.tick();

    // SmsSender should have the entry
    TEST_ASSERT_EQUAL(1, sender.queueSize());
    auto snaps = sender.getQueueSnapshot();
    TEST_ASSERT_EQUAL(1, (int)snaps.size());
    TEST_ASSERT_EQUAL_STRING("+13800138000", snaps[0].phone.c_str());
}

// RFC-0188: /schedqueue lists pending slots.
void test_TelegramPoller_schedqueue_lists_pending()
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
                          allowedAuth);
    poller.begin();
    poller.tick(); // consume initial poll

    // Schedule one SMS
    bot.clearMessages();
    bot.queueUpdateBatch({makeUpdate(994, kAllowedFromId, 0,
        "/schedulesend 5 +13800138000 Hello", kAllowedFromId)});
    clk.nowMs += 4000;
    poller.tick();

    bot.clearMessages();
    bot.queueUpdateBatch({makeUpdate(995, kAllowedFromId, 0, "/schedqueue", kAllowedFromId)});
    clk.nowMs += 4000;
    poller.tick();

    bool sawPhone = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("+13800138000") >= 0) { sawPhone = true; break; }
    TEST_ASSERT_TRUE(sawPhone);
}

// RFC-0188: /cancelsched 1 clears the slot.
void test_TelegramPoller_cancelsched_clears_slot()
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
                          allowedAuth);
    poller.begin();
    poller.tick();

    // Schedule one SMS
    bot.clearMessages();
    bot.queueUpdateBatch({makeUpdate(996, kAllowedFromId, 0,
        "/schedulesend 10 +13800138000 Bye", kAllowedFromId)});
    clk.nowMs += 4000;
    poller.tick();

    // Cancel it
    bot.clearMessages();
    bot.queueUpdateBatch({makeUpdate(997, kAllowedFromId, 0, "/cancelsched 1", kAllowedFromId)});
    clk.nowMs += 4000;
    poller.tick();

    bool sawCancelled = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("cancelled") >= 0 || m.indexOf("Cancelled") >= 0)
            { sawCancelled = true; break; }
    TEST_ASSERT_TRUE(sawCancelled);

    // Advance past delay — nothing should be sent
    clk.nowMs += 620000UL;
    poller.tick();
    TEST_ASSERT_EQUAL(0, sender.queueSize());
}

// RFC-0137: /setinterval 3600 calls fn with 3600.
void test_TelegramPoller_setinterval_calls_fn()
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

    uint32_t capturedInterval = 0;
    poller.setIntervalFn([&](uint32_t secs) { capturedInterval = secs; });

    bot.queueUpdateBatch({makeUpdate(993, kAllowedFromId, 0, "/setinterval 3600", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(993, poller.lastUpdateId());
    TEST_ASSERT_EQUAL((uint32_t)3600, capturedInterval);
    bool sawConfirm = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("3600")) >= 0) { sawConfirm = true; break; }
    }
    TEST_ASSERT_TRUE(sawConfirm);
}

// RFC-0137: /setinterval 0 → disable confirmed.
void test_TelegramPoller_setinterval_zero_disables()
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

    uint32_t capturedInterval = 999;
    poller.setIntervalFn([&](uint32_t secs) { capturedInterval = secs; });

    bot.queueUpdateBatch({makeUpdate(994, kAllowedFromId, 0, "/setinterval 0", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(994, poller.lastUpdateId());
    TEST_ASSERT_EQUAL((uint32_t)0, capturedInterval);
    bool sawDisabled = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("disabled")) >= 0) { sawDisabled = true; break; }
    }
    TEST_ASSERT_TRUE(sawDisabled);
}

// RFC-0137: /setinterval 99999 → validation error.
void test_TelegramPoller_setinterval_too_large_sends_error()
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
    poller.setIntervalFn([](uint32_t) {});

    bot.queueUpdateBatch({makeUpdate(995, kAllowedFromId, 0, "/setinterval 99999", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(995, poller.lastUpdateId());
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Invalid")) >= 0) { sawError = true; break; }
    }
    TEST_ASSERT_TRUE(sawError);
}

// RFC-0177: /hbnow calls heartbeatNowFn and replies success when enabled.
void test_TelegramPoller_hbnow_calls_fn_and_replies_success()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool called = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setHeartbeatNowFn([&called]() -> bool { called = true; return true; });

    bot.queueUpdateBatch({makeUpdate(1098, kAllowedFromId, 0, "/hbnow", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_TRUE(called);
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("triggered") >= 0) { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0177: /hbnow replies disabled message when fn returns false.
void test_TelegramPoller_hbnow_replies_disabled_when_fn_returns_false()
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
    poller.setHeartbeatNowFn([]() -> bool { return false; }); // disabled

    bot.queueUpdateBatch({makeUpdate(1099, kAllowedFromId, 0, "/hbnow", kAllowedFromId)});
    poller.tick();

    bool sawDisabled = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("disabled") >= 0) { sawDisabled = true; break; }
    TEST_ASSERT_TRUE(sawDisabled);
}

// RFC-0134: /clearaliases with populated store → removes all and confirms count.
void test_TelegramPoller_clearaliases_removes_all()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsAliasStore store(persist);
    store.set(String("alice"), String("+447911123456"));
    store.set(String("bob"),   String("+447911654321"));

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setAliasStore(&store);

    bot.queueUpdateBatch({makeUpdate(980, kAllowedFromId, 0, "/clearaliases", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(980, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(0, store.count());
    bool sawCleared = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("Cleared")) >= 0 && m.indexOf(String("2")) >= 0)
        {
            sawCleared = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawCleared);
}

// RFC-0134: /clearaliases with empty store → "(no aliases)".
void test_TelegramPoller_clearaliases_empty_store_replies_placeholder()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsAliasStore store(persist);

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setAliasStore(&store);

    bot.queueUpdateBatch({makeUpdate(981, kAllowedFromId, 0, "/clearaliases", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(981, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
    {
        if (m.indexOf(String("no aliases")) >= 0) { sawPlaceholder = true; break; }
    }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0186: /importaliases with valid name=phone lines → all imported.
void test_TelegramPoller_importaliases_valid_lines_imported()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsAliasStore store(persist);

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setAliasStore(&store);

    // Multi-line message: command + two aliases
    bot.queueUpdateBatch({makeUpdate(1106, kAllowedFromId, 0,
        "/importaliases\nAlice=+13800138000\nBob=+14155551234", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1106, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(2, store.count());

    TEST_ASSERT_EQUAL_STRING("+13800138000", store.lookup("Alice").c_str());
    TEST_ASSERT_EQUAL_STRING("+14155551234", store.lookup("Bob").c_str());

    bool sawImported = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("Imported") >= 0 && m.indexOf("2") >= 0) { sawImported = true; break; }
    TEST_ASSERT_TRUE(sawImported);
}

// RFC-0186: /importaliases with some invalid lines → imported + skipped count.
void test_TelegramPoller_importaliases_skips_invalid_lines()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsAliasStore store(persist);

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setAliasStore(&store);

    // One valid, one invalid (no '='), one valid
    bot.queueUpdateBatch({makeUpdate(1107, kAllowedFromId, 0,
        "/importaliases\nAlice=+13800138000\nbadline\nCharlie=+15005550006",
        kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1107, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(2, store.count());

    // Reply should mention "skipped 1"
    bool sawSkipped = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("skipped") >= 0 && m.indexOf("1") >= 0) { sawSkipped = true; break; }
    TEST_ASSERT_TRUE(sawSkipped);
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

// RFC-0152: /resetwatermark resets lastUpdateId to 0 and persists.
void test_TelegramPoller_resetwatermark_resets_to_zero()
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

    // Advance watermark to 500.
    bot.queueUpdateBatch({makeUpdate(500, kAllowedFromId, 0, "/ping", kAllowedFromId)});
    poller.tick();
    TEST_ASSERT_EQUAL(500, poller.lastUpdateId());

    // Reset.
    clk.nowMs += TelegramPoller::kPollIntervalMs;
    bot.clearMessages();
    bot.queueUpdateBatch({makeUpdate(501, kAllowedFromId, 0, "/resetwatermark", kAllowedFromId)});
    poller.tick();

    // After reset the watermark goes to 0 before processing the current update.
    // The reset happens inside processUpdate (the /resetwatermark handler),
    // so the returned lastUpdateId reflects the current update (501) after re-processing.
    // Key: persisted value should be 0, and a confirmation reply was sent.
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("reset")) >= 0 || m.indexOf(String("Reset")) >= 0)
            { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0153: /setforward off calls fn with false.
void test_TelegramPoller_setforward_off_calls_fn_false()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool captured = true;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setForwardingEnabledFn([&captured](bool b) { captured = b; });

    bot.queueUpdateBatch({makeUpdate(1060, kAllowedFromId, 0, "/setforward off", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1060, poller.lastUpdateId());
    TEST_ASSERT_FALSE(captured);
    bool sawPaused = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("PAUSED")) >= 0 || m.indexOf(String("paused")) >= 0)
            { sawPaused = true; break; }
    TEST_ASSERT_TRUE(sawPaused);
}

// RFC-0153: /setforward on calls fn with true.
void test_TelegramPoller_setforward_on_calls_fn_true()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool captured = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setForwardingEnabledFn([&captured](bool b) { captured = b; });

    bot.queueUpdateBatch({makeUpdate(1061, kAllowedFromId, 0, "/setforward on", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1061, poller.lastUpdateId());
    TEST_ASSERT_TRUE(captured);
    bool sawEnabled = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("enabled")) >= 0) { sawEnabled = true; break; }
    TEST_ASSERT_TRUE(sawEnabled);
}

// RFC-0151: /getautoreply shows current auto-reply text.
void test_TelegramPoller_getautoreply_shows_text()
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
    poller.setAutoReplyGetFn([]() -> String { return String("Reply via Telegram"); });

    bot.queueUpdateBatch({makeUpdate(1050, kAllowedFromId, 0, "/getautoreply", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1050, poller.lastUpdateId());
    bool sawText = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Reply via Telegram")) >= 0) { sawText = true; break; }
    TEST_ASSERT_TRUE(sawText);
}

// RFC-0151: /setautoreply <text> calls setter.
void test_TelegramPoller_setautoreply_calls_setter()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    String captured;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setAutoReplyGetFn([]() -> String { return String(); });
    poller.setAutoReplySetFn([&captured](const String &t) { captured = t; });

    bot.queueUpdateBatch({makeUpdate(1051, kAllowedFromId, 0,
        "/setautoreply I will reply soon", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1051, poller.lastUpdateId());
    TEST_ASSERT_TRUE(captured.indexOf(String("I will reply soon")) >= 0);
}

// RFC-0151: /clearautoreply clears the text.
void test_TelegramPoller_clearautoreply_calls_setter_empty()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool setterCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setAutoReplyGetFn([]() -> String { return String(); });
    poller.setAutoReplySetFn([&setterCalled](const String &t) {
        if (t.length() == 0) setterCalled = true;
    });

    bot.queueUpdateBatch({makeUpdate(1052, kAllowedFromId, 0, "/clearautoreply", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1052, poller.lastUpdateId());
    TEST_ASSERT_TRUE(setterCalled);
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("cleared")) >= 0 || m.indexOf(String("Cleared")) >= 0)
            { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0148: /sweepsim calls fn and reports count.
void test_TelegramPoller_sweepsim_calls_fn_and_reports()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setSweepFn([&fnCalled]() -> int { fnCalled = true; return 3; });

    bot.queueUpdateBatch({makeUpdate(1040, kAllowedFromId, 0, "/sweepsim", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1040, poller.lastUpdateId());
    TEST_ASSERT_TRUE(fnCalled);
    bool sawCount = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("3")) >= 0 && m.indexOf(String("wept")) >= 0) { sawCount = true; break; }
    TEST_ASSERT_TRUE(sawCount);
}

// RFC-0148: /sweepsim with 0 SMS → placeholder.
void test_TelegramPoller_sweepsim_zero_sms_replies_placeholder()
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
    poller.setSweepFn([]() -> int { return 0; });

    bot.queueUpdateBatch({makeUpdate(1041, kAllowedFromId, 0, "/sweepsim", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1041, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("no SMS")) >= 0 || m.indexOf(String("No SMS")) >= 0)
            { sawPlaceholder = true; break; }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0149: /health calls fn and forwards result.
void test_TelegramPoller_health_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setHealthFn([&fnCalled]() -> String {
        fnCalled = true;
        return String("\xe2\x9c\x85 OK | WiFi: -65dBm | CSQ: 18");
    });

    bot.queueUpdateBatch({makeUpdate(1042, kAllowedFromId, 0, "/health", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1042, poller.lastUpdateId());
    TEST_ASSERT_TRUE(fnCalled);
    bool sawResult = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("CSQ")) >= 0) { sawResult = true; break; }
    TEST_ASSERT_TRUE(sawResult);
}

// RFC-0146: /forwardsim <idx> calls fn with index.
void test_TelegramPoller_forwardsim_calls_fn_with_index()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    int capturedIdx = -1;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setSmsForwardFn([&capturedIdx](int idx) -> bool { capturedIdx = idx; return true; });

    bot.queueUpdateBatch({makeUpdate(1030, kAllowedFromId, 0, "/forwardsim 12", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1030, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(12, capturedIdx);
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("12")) >= 0 && m.indexOf(String("orward")) >= 0) { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0146: /forwardsim with no arg → usage error.
void test_TelegramPoller_forwardsim_no_arg_sends_usage()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setSmsForwardFn([&fnCalled](int) -> bool { fnCalled = true; return true; });

    bot.queueUpdateBatch({makeUpdate(1031, kAllowedFromId, 0, "/forwardsim", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1031, poller.lastUpdateId());
    TEST_ASSERT_FALSE(fnCalled);
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Usage")) >= 0) { sawUsage = true; break; }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0147: /setpollinterval 10 sets the poll interval.
void test_TelegramPoller_setpollinterval_updates_interval()
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

    bot.queueUpdateBatch({makeUpdate(1032, kAllowedFromId, 0, "/setpollinterval 10", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1032, poller.lastUpdateId());
    // After setting to 10s, advancing clock by 5s should not trigger another poll.
    bot.clearMessages();
    clk.nowMs += 5000;
    bot.queueUpdateBatch({makeUpdate(1033, kAllowedFromId, 0, "/ping", kAllowedFromId)});
    poller.tick();
    // 5s < 10s interval, so no poll should have occurred.
    TEST_ASSERT_EQUAL(1032, poller.lastUpdateId()); // watermark unchanged
}

// RFC-0147: /setpollinterval 0 → validation error.
void test_TelegramPoller_setpollinterval_zero_sends_error()
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

    bot.queueUpdateBatch({makeUpdate(1033, kAllowedFromId, 0, "/setpollinterval 0", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1033, poller.lastUpdateId());
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Invalid")) >= 0) { sawError = true; break; }
    TEST_ASSERT_TRUE(sawError);
}

// RFC-0144: /setdedup <seconds> calls fn.
void test_TelegramPoller_setdedup_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    uint32_t captured = 999;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setDedupWindowFn([&captured](uint32_t secs) { captured = secs; });

    bot.queueUpdateBatch({makeUpdate(1020, kAllowedFromId, 0, "/setdedup 60", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1020, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(60u, captured);
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("60")) >= 0) { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0144: /setdedup 0 disables dedup.
void test_TelegramPoller_setdedup_zero_disables()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    uint32_t captured = 999;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setDedupWindowFn([&captured](uint32_t secs) { captured = secs; });

    bot.queueUpdateBatch({makeUpdate(1021, kAllowedFromId, 0, "/setdedup 0", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1021, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(0u, captured);
    bool sawDisabled = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("disabled")) >= 0) { sawDisabled = true; break; }
    TEST_ASSERT_TRUE(sawDisabled);
}

// RFC-0145: /cleardedup calls fn.
void test_TelegramPoller_cleardedup_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setClearDedupFn([&fnCalled]() { fnCalled = true; });

    bot.queueUpdateBatch({makeUpdate(1022, kAllowedFromId, 0, "/cleardedup", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1022, poller.lastUpdateId());
    TEST_ASSERT_TRUE(fnCalled);
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("cleared")) >= 0 || m.indexOf(String("Cleared")) >= 0)
            { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0142: /setconcatttl <seconds> calls fn.
void test_TelegramPoller_setconcatttl_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    uint32_t captured = 0;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setConcatTtlFn([&captured](uint32_t secs) { captured = secs; });

    bot.queueUpdateBatch({makeUpdate(1010, kAllowedFromId, 0, "/setconcatttl 7200", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1010, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(7200u, captured);
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("7200")) >= 0) { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0142: /setconcatttl 59 → validation error (< 60).
void test_TelegramPoller_setconcatttl_too_small_sends_error()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setConcatTtlFn([&fnCalled](uint32_t) { fnCalled = true; });

    bot.queueUpdateBatch({makeUpdate(1011, kAllowedFromId, 0, "/setconcatttl 59", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1011, poller.lastUpdateId());
    TEST_ASSERT_FALSE(fnCalled);
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Invalid")) >= 0) { sawError = true; break; }
    TEST_ASSERT_TRUE(sawError);
}

// RFC-0143: /modeminfo calls fn and forwards result.
void test_TelegramPoller_modeminfo_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setModemInfoFn([&fnCalled]() -> String {
        fnCalled = true;
        return String("IMEI: 123456789012345\nModel: A7670G\nFW: 1.2.3");
    });

    bot.queueUpdateBatch({makeUpdate(1012, kAllowedFromId, 0, "/modeminfo", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1012, poller.lastUpdateId());
    TEST_ASSERT_TRUE(fnCalled);
    bool sawImei = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("IMEI")) >= 0) { sawImei = true; break; }
    TEST_ASSERT_TRUE(sawImei);
}

// RFC-0143: /modeminfo not configured → placeholder.
void test_TelegramPoller_modeminfo_not_configured_replies_placeholder()
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

    bot.queueUpdateBatch({makeUpdate(1013, kAllowedFromId, 0, "/modeminfo", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1013, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("not configured")) >= 0) { sawPlaceholder = true; break; }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0140: /simlist calls fn and forwards result.
void test_TelegramPoller_simlist_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setSimListFn([&fnCalled]() -> String {
        fnCalled = true;
        return String("\xF0\x9F\x93\x8B SIM: 3 stored\n  [1]\n  [3]\n  [7]\n");
    });

    bot.queueUpdateBatch({makeUpdate(1000, kAllowedFromId, 0, "/simlist", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1000, poller.lastUpdateId());
    TEST_ASSERT_TRUE(fnCalled);
    bool sawResult = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("SIM")) >= 0) { sawResult = true; break; }
    TEST_ASSERT_TRUE(sawResult);
}

// RFC-0140: /simlist not configured → placeholder.
void test_TelegramPoller_simlist_not_configured_replies_placeholder()
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
    // setSimListFn NOT called → fn is nullptr.

    bot.queueUpdateBatch({makeUpdate(1001, kAllowedFromId, 0, "/simlist", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1001, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("not configured")) >= 0) { sawPlaceholder = true; break; }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0141: /simread <idx> calls fn with idx and forwards result.
void test_TelegramPoller_simread_calls_fn_with_index()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    int capturedIdx = -1;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setSimReadFn([&capturedIdx](int idx) -> String {
        capturedIdx = idx;
        return String("From: +447911123456\nBody: Hello");
    });

    bot.queueUpdateBatch({makeUpdate(1002, kAllowedFromId, 0, "/simread 7", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1002, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(7, capturedIdx);
    bool sawResult = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Hello")) >= 0) { sawResult = true; break; }
    TEST_ASSERT_TRUE(sawResult);
}

// RFC-0141: /simread with no arg → usage error.
void test_TelegramPoller_simread_no_arg_sends_usage()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setSimReadFn([&fnCalled](int) -> String { fnCalled = true; return String(); });

    bot.queueUpdateBatch({makeUpdate(1003, kAllowedFromId, 0, "/simread", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1003, poller.lastUpdateId());
    TEST_ASSERT_FALSE(fnCalled);
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Usage")) >= 0) { sawUsage = true; break; }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0141: /simread 0 → validation error (idx < 1).
void test_TelegramPoller_simread_invalid_index_sends_error()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setSimReadFn([&fnCalled](int) -> String { fnCalled = true; return String(); });

    bot.queueUpdateBatch({makeUpdate(1004, kAllowedFromId, 0, "/simread 0", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1004, poller.lastUpdateId());
    TEST_ASSERT_FALSE(fnCalled);
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Invalid")) >= 0) { sawError = true; break; }
    TEST_ASSERT_TRUE(sawError);
}

// RFC-0138: /setmaxfail <N> calls fn with N.
void test_TelegramPoller_setmaxfail_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    int captured = -1;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setMaxFailFn([&captured](int n) { captured = n; });

    bot.queueUpdateBatch({makeUpdate(990, kAllowedFromId, 0, "/setmaxfail 5", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(990, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(5, captured);
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("5")) >= 0 && m.indexOf(String("failures")) >= 0) { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0138: /setmaxfail 0 disables reboot.
void test_TelegramPoller_setmaxfail_zero_disables()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    int captured = -1;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setMaxFailFn([&captured](int n) { captured = n; });

    bot.queueUpdateBatch({makeUpdate(991, kAllowedFromId, 0, "/setmaxfail 0", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(991, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(0, captured);
    bool sawDisabled = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("disabled")) >= 0) { sawDisabled = true; break; }
    TEST_ASSERT_TRUE(sawDisabled);
}

// RFC-0138: /setmaxfail 100 → validation error.
void test_TelegramPoller_setmaxfail_too_large_sends_error()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setMaxFailFn([&fnCalled](int) { fnCalled = true; });

    bot.queueUpdateBatch({makeUpdate(992, kAllowedFromId, 0, "/setmaxfail 100", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(992, poller.lastUpdateId());
    TEST_ASSERT_FALSE(fnCalled);
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Invalid")) >= 0) { sawError = true; break; }
    TEST_ASSERT_TRUE(sawError);
}

// RFC-0139: /flushsim yes calls fn and confirms.
void test_TelegramPoller_flushsim_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setFlushSimFn([&fnCalled]() -> int { fnCalled = true; return 7; });

    bot.queueUpdateBatch({makeUpdate(993, kAllowedFromId, 0, "/flushsim yes", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(993, poller.lastUpdateId());
    TEST_ASSERT_TRUE(fnCalled);
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("flushed")) >= 0) { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0139: /flushsim without "yes" → usage error.
void test_TelegramPoller_flushsim_without_yes_sends_usage()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setFlushSimFn([&fnCalled]() -> int { fnCalled = true; return 0; });

    bot.queueUpdateBatch({makeUpdate(994, kAllowedFromId, 0, "/flushsim", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(994, poller.lastUpdateId());
    TEST_ASSERT_FALSE(fnCalled);
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("yes")) >= 0) { sawUsage = true; break; }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0154: /logstats — aggregate stats from the debug log.
void test_TelegramPoller_logstats_returns_summary()
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

    SmsDebugLog log;
    {
        SmsDebugLog::Entry e;
        e.sender = "+1111";
        e.outcome = "fwd OK";
        log.push(e);
    }
    {
        SmsDebugLog::Entry e;
        e.sender = "+2222";
        e.outcome = "fwd FAIL";
        log.push(e);
    }
    {
        SmsDebugLog::Entry e;
        e.sender = "+3333";
        e.outcome = "buffered";
        log.push(e);
    }
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1062, kAllowedFromId, 0, "/logstats", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1062, poller.lastUpdateId());
    bool sawStats = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("forwarded")) >= 0 && m.indexOf(String("failed")) >= 0)
            { sawStats = true; break; }
    TEST_ASSERT_TRUE(sawStats);
}

// RFC-0154: /logstats without debug log → placeholder.
void test_TelegramPoller_logstats_no_log_replies_placeholder()
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
    // No setDebugLog call.

    bot.queueUpdateBatch({makeUpdate(1063, kAllowedFromId, 0, "/logstats", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1063, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("not configured")) >= 0) { sawPlaceholder = true; break; }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0155: /logsoutcome <keyword> — filter log entries by outcome substring.
void test_TelegramPoller_logsoutcome_returns_matching_entries()
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

    SmsDebugLog log;
    {
        SmsDebugLog::Entry e;
        e.sender = "+1111";
        e.outcome = "fwd OK";
        log.push(e);
    }
    {
        SmsDebugLog::Entry e;
        e.sender = "+2222";
        e.outcome = "fwd FAIL";
        log.push(e);
    }
    {
        SmsDebugLog::Entry e;
        e.sender = "+3333";
        e.outcome = "dup";
        log.push(e);
    }
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1064, kAllowedFromId, 0, "/logsoutcome FAIL", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1064, poller.lastUpdateId());
    bool sawFail = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("FAIL")) >= 0 && m.indexOf(String("+2222")) >= 0)
            { sawFail = true; break; }
    TEST_ASSERT_TRUE(sawFail);
}

// RFC-0155: /logsoutcome with no matching entries → placeholder.
void test_TelegramPoller_logsoutcome_no_match_replies_placeholder()
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

    SmsDebugLog log;
    {
        SmsDebugLog::Entry e;
        e.sender = "+1111";
        e.outcome = "fwd OK";
        log.push(e);
    }
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1065, kAllowedFromId, 0, "/logsoutcome dup", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1065, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("no entries")) >= 0) { sawPlaceholder = true; break; }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0155: /logsoutcome with no arg → usage message.
void test_TelegramPoller_logsoutcome_no_arg_sends_usage()
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
    SmsDebugLog log;
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1066, kAllowedFromId, 0, "/logsoutcome", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1066, poller.lastUpdateId());
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Usage")) >= 0) { sawUsage = true; break; }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0156: /simstatus — calls simStatusFn and sends result.
void test_TelegramPoller_simstatus_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setSimStatusFn([&fnCalled]() -> String {
        fnCalled = true;
        return String("Reg: registered (home)\nCSQ: 18\nOperator: Test Net");
    });

    bot.queueUpdateBatch({makeUpdate(1067, kAllowedFromId, 0, "/simstatus", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1067, poller.lastUpdateId());
    TEST_ASSERT_TRUE(fnCalled);
    bool sawResult = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("registered")) >= 0) { sawResult = true; break; }
    TEST_ASSERT_TRUE(sawResult);
}

// RFC-0156: /simstatus without fn configured → placeholder.
void test_TelegramPoller_simstatus_not_configured_replies_placeholder()
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
    // No setSimStatusFn call.

    bot.queueUpdateBatch({makeUpdate(1068, kAllowedFromId, 0, "/simstatus", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1068, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("not configured")) >= 0) { sawPlaceholder = true; break; }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0157: /topn — returns top N senders by frequency.
void test_TelegramPoller_topn_returns_top_senders()
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

    SmsDebugLog log;
    auto push = [&](const char *s) {
        SmsDebugLog::Entry e; e.sender = String(s); e.outcome = "fwd OK"; log.push(e);
    };
    push("+1111"); push("+2222"); push("+1111"); push("+3333"); push("+1111");
    // +1111: 3, +2222: 1, +3333: 1
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1069, kAllowedFromId, 0, "/topn 2", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1069, poller.lastUpdateId());
    bool sawTop = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("+1111")) >= 0 && m.indexOf(String("3")) >= 0)
            { sawTop = true; break; }
    TEST_ASSERT_TRUE(sawTop);
}

// RFC-0157: /topn with no debug log → placeholder.
void test_TelegramPoller_topn_no_log_replies_placeholder()
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

    bot.queueUpdateBatch({makeUpdate(1070, kAllowedFromId, 0, "/topn", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1070, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("not configured")) >= 0) { sawPlaceholder = true; break; }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0158: /wifiscan — calls wifiScanFn and sends result.
void test_TelegramPoller_wifiscan_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setWifiScanFn([&fnCalled]() -> String {
        fnCalled = true;
        return String("MySSID ch6 -55dBm\nOtherNet ch11 -78dBm");
    });

    bot.queueUpdateBatch({makeUpdate(1071, kAllowedFromId, 0, "/wifiscan", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1071, poller.lastUpdateId());
    TEST_ASSERT_TRUE(fnCalled);
    bool sawResult = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("MySSID")) >= 0) { sawResult = true; break; }
    TEST_ASSERT_TRUE(sawResult);
}

// RFC-0158: /wifiscan without fn configured → placeholder.
void test_TelegramPoller_wifiscan_not_configured_replies_placeholder()
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

    bot.queueUpdateBatch({makeUpdate(1072, kAllowedFromId, 0, "/wifiscan", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1072, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("not configured")) >= 0) { sawPlaceholder = true; break; }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0159: /logsince — show entries from the past N hours.
// This test pushes entries with known unixTimestamps and verifies the
// dumpBriefSince filter works correctly. The TelegramPoller handler uses
// time(nullptr) which we can't easily mock, so we test /logsince no-arg
// and bad-arg paths via the poller; the core filter logic is tested in
// test_main.cpp (SmsDebugLog unit tests).
void test_TelegramPoller_logsince_no_arg_sends_usage()
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
    SmsDebugLog log;
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1073, kAllowedFromId, 0, "/logsince", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1073, poller.lastUpdateId());
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Usage")) >= 0) { sawUsage = true; break; }
    TEST_ASSERT_TRUE(sawUsage);
}

void test_TelegramPoller_logsince_invalid_hours_sends_error()
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
    SmsDebugLog log;
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1074, kAllowedFromId, 0, "/logsince 999", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1074, poller.lastUpdateId());
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Error")) >= 0 || m.indexOf(String("168")) >= 0)
            { sawError = true; break; }
    TEST_ASSERT_TRUE(sawError);
}

// RFC-0179: /logcsv — exports debug log as CSV with header.
void test_TelegramPoller_logcsv_returns_csv_with_header()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsDebugLog log;
    { SmsDebugLog::Entry e; e.unixTimestamp = 1775606400; e.sender = "+55555";
      e.outcome = "fwd OK"; e.bodyChars = 10; log.push(e); }

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1102, kAllowedFromId, 0, "/logcsv", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1102, poller.lastUpdateId());
    bool sawHeader = false, sawData = false;
    for (const auto &m : bot.sentMessages()) {
        if (m.indexOf("unix_ts,sender,outcome,chars") >= 0) sawHeader = true;
        if (m.indexOf("+55555") >= 0) sawData = true;
    }
    TEST_ASSERT_TRUE(sawHeader);
    TEST_ASSERT_TRUE(sawData);
}

// RFC-0178: /logdate YYYY-MM-DD — valid date calls dumpBriefRange.
void test_TelegramPoller_logdate_valid_date_calls_debug_log()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsDebugLog log;
    // 2026-04-08 00:00:00 UTC = 1775606400.
    // Add an entry inside and outside that window.
    { SmsDebugLog::Entry e; e.unixTimestamp = 1775606400 + 3600; // 2026-04-08 01:00 UTC
      e.sender = "+99001"; e.outcome = "fwd OK"; e.bodyChars = 5; log.push(e); }
    { SmsDebugLog::Entry e; e.unixTimestamp = 1775606400 - 100;  // 2026-04-07
      e.sender = "+99002"; e.outcome = "fwd OK"; e.bodyChars = 5; log.push(e); }

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1100, kAllowedFromId, 0, "/logdate 2026-04-08", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1100, poller.lastUpdateId());
    bool sawEntry = false, sawWrong = false;
    for (const auto &m : bot.sentMessages()) {
        if (m.indexOf("+99001") >= 0) sawEntry = true;
        if (m.indexOf("+99002") >= 0) sawWrong = true;
    }
    TEST_ASSERT_TRUE(sawEntry);
    TEST_ASSERT_FALSE(sawWrong);
}

// RFC-0178: /logdate invalid format sends error.
void test_TelegramPoller_logdate_invalid_format_sends_error()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsDebugLog log;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1101, kAllowedFromId, 0, "/logdate notadate", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1101, poller.lastUpdateId());
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("YYYY-MM-DD") >= 0) { sawUsage = true; break; }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0160: /setmaxparts <N> — calls maxPartsFn with validated value.
void test_TelegramPoller_setmaxparts_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    int captured = -1;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setMaxPartsFn([&captured](int n) { captured = n; });

    bot.queueUpdateBatch({makeUpdate(1075, kAllowedFromId, 0, "/setmaxparts 3", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1075, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(3, captured);
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("3")) >= 0 && m.indexOf(String("parts")) >= 0) { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0160: /setmaxparts out of range → error.
void test_TelegramPoller_setmaxparts_out_of_range_sends_error()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setMaxPartsFn([&fnCalled](int) { fnCalled = true; });

    bot.queueUpdateBatch({makeUpdate(1076, kAllowedFromId, 0, "/setmaxparts 11", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1076, poller.lastUpdateId());
    TEST_ASSERT_FALSE(fnCalled);
    bool sawError = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Error")) >= 0) { sawError = true; break; }
    TEST_ASSERT_TRUE(sawError);
}

// RFC-0161: /smscount — calls smsCntFn and sends result.
void test_TelegramPoller_smscount_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool fnCalled = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setSmsCntFn([&fnCalled]() -> String {
        fnCalled = true;
        return String("SM: 3/20");
    });

    bot.queueUpdateBatch({makeUpdate(1077, kAllowedFromId, 0, "/smscount", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1077, poller.lastUpdateId());
    TEST_ASSERT_TRUE(fnCalled);
    bool sawResult = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("SM")) >= 0 && m.indexOf(String("3")) >= 0) { sawResult = true; break; }
    TEST_ASSERT_TRUE(sawResult);
}

// RFC-0161: /smscount not configured → placeholder.
void test_TelegramPoller_smscount_not_configured_replies_placeholder()
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

    bot.queueUpdateBatch({makeUpdate(1078, kAllowedFromId, 0, "/smscount", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1078, poller.lastUpdateId());
    bool sawPlaceholder = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("not configured")) >= 0) { sawPlaceholder = true; break; }
    TEST_ASSERT_TRUE(sawPlaceholder);
}

// RFC-0162: /setblockmode off calls fn with false.
void test_TelegramPoller_setblockmode_off_calls_fn_false()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool captured = true;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setBlockingEnabledFn([&captured](bool v) { captured = v; });

    bot.queueUpdateBatch({makeUpdate(1079, kAllowedFromId, 0, "/setblockmode off", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1079, poller.lastUpdateId());
    TEST_ASSERT_FALSE(captured);
    bool sawSuspend = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("SUSPENDED")) >= 0 || m.indexOf(String("pass through")) >= 0)
            { sawSuspend = true; break; }
    TEST_ASSERT_TRUE(sawSuspend);
}

// RFC-0162: /setblockmode on calls fn with true.
void test_TelegramPoller_setblockmode_on_calls_fn_true()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool captured = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setBlockingEnabledFn([&captured](bool v) { captured = v; });

    bot.queueUpdateBatch({makeUpdate(1080, kAllowedFromId, 0, "/setblockmode on", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1080, poller.lastUpdateId());
    TEST_ASSERT_TRUE(captured);
}

// RFC-0163: /blockcheck <phone> — calls blockCheckFn with phone number.
void test_TelegramPoller_blockcheck_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    String captured;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setBlockCheckFn([&captured](const String &ph) -> String {
        captured = ph;
        return String("NOT blocked");
    });

    bot.queueUpdateBatch({makeUpdate(1081, kAllowedFromId, 0, "/blockcheck +1234567890", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1081, poller.lastUpdateId());
    TEST_ASSERT_TRUE(captured == String("+1234567890"));
    bool sawResult = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("NOT blocked")) >= 0) { sawResult = true; break; }
    TEST_ASSERT_TRUE(sawResult);
}

// RFC-0163: /blockcheck without arg → usage.
void test_TelegramPoller_blockcheck_no_arg_sends_usage()
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
    poller.setBlockCheckFn([](const String &) -> String { return String("x"); });

    bot.queueUpdateBatch({makeUpdate(1082, kAllowedFromId, 0, "/blockcheck", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1082, poller.lastUpdateId());
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("Usage")) >= 0) { sawUsage = true; break; }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0164: /setcallnotify off calls fn with false.
void test_TelegramPoller_setcallnotify_off_calls_fn_false()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool captured = true;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setCallNotifyFn([&captured](bool v) { captured = v; });

    bot.queueUpdateBatch({makeUpdate(1083, kAllowedFromId, 0, "/setcallnotify off", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1083, poller.lastUpdateId());
    TEST_ASSERT_FALSE(captured);
    bool sawMuted = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf(String("MUTED")) >= 0 || m.indexOf(String("muted")) >= 0
            || m.indexOf(String("auto-rejected")) >= 0)
            { sawMuted = true; break; }
    TEST_ASSERT_TRUE(sawMuted);
}

// RFC-0164: /setcallnotify on calls fn with true.
void test_TelegramPoller_setcallnotify_on_calls_fn_true()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool captured = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setCallNotifyFn([&captured](bool v) { captured = v; });

    bot.queueUpdateBatch({makeUpdate(1084, kAllowedFromId, 0, "/setcallnotify on", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1084, poller.lastUpdateId());
    TEST_ASSERT_TRUE(captured);
}

// RFC-0165: /setcalldedup command
void test_TelegramPoller_setcalldedup_calls_fn_with_ms()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    uint32_t captured = 0;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setCallDedupFn([&captured](uint32_t ms) { captured = ms; });

    bot.queueUpdateBatch({makeUpdate(1085, kAllowedFromId, 0, "/setcalldedup 10", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1085, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(10000, captured);
}

void test_TelegramPoller_setcalldedup_out_of_range_replies_error()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    uint32_t captured = 0;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setCallDedupFn([&captured](uint32_t ms) { captured = ms; });

    bot.queueUpdateBatch({makeUpdate(1086, kAllowedFromId, 0, "/setcalldedup 99", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1086, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(0, captured); // fn not called
    auto msgs = bot.sentMessages();
    TEST_ASSERT_TRUE(msgs.size() > 0);
    TEST_ASSERT_TRUE(msgs.back().indexOf("Error") >= 0);
}

// RFC-0166: /setunknowndeadline command
void test_TelegramPoller_setunknowndeadline_calls_fn_with_ms()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    uint32_t captured = 0;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setCallUnknownDeadlineFn([&captured](uint32_t ms) { captured = ms; });

    bot.queueUpdateBatch({makeUpdate(1087, kAllowedFromId, 0, "/setunknowndeadline 3000", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1087, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(3000, captured);
}

void test_TelegramPoller_setunknowndeadline_out_of_range_replies_error()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    uint32_t captured = 0;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setCallUnknownDeadlineFn([&captured](uint32_t ms) { captured = ms; });

    bot.queueUpdateBatch({makeUpdate(1088, kAllowedFromId, 0, "/setunknowndeadline 100", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1088, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(0, captured); // fn not called
    auto msgs = bot.sentMessages();
    TEST_ASSERT_TRUE(msgs.size() > 0);
    TEST_ASSERT_TRUE(msgs.back().indexOf("Error") >= 0);
}

// RFC-0167: /settings command
void test_TelegramPoller_settings_calls_fn_and_replies()
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
    poller.setSettingsFn([]() -> String { return "settings: forwarding=ON"; });

    bot.queueUpdateBatch({makeUpdate(1089, kAllowedFromId, 0, "/settings", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1089, poller.lastUpdateId());
    auto msgs = bot.sentMessages();
    TEST_ASSERT_TRUE(msgs.size() > 0);
    TEST_ASSERT_TRUE(msgs.back().indexOf("forwarding=ON") >= 0);
}

// RFC-0168: /nvsinfo command
void test_TelegramPoller_nvsinfo_calls_fn_and_replies()
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
    poller.setNvsInfoFn([]() -> String { return "NVS: used=12 free=500 total=512"; });

    bot.queueUpdateBatch({makeUpdate(1090, kAllowedFromId, 0, "/nvsinfo", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1090, poller.lastUpdateId());
    auto msgs = bot.sentMessages();
    TEST_ASSERT_TRUE(msgs.size() > 0);
    TEST_ASSERT_TRUE(msgs.back().indexOf("used=12") >= 0);
}

// RFC-0169/0175: /setgmtoffset command — fn receives minutes (hours * 60)
void test_TelegramPoller_setgmtoffset_calls_fn_with_hours()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    int captured = 999;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setGmtOffsetFn([&captured](int m) { captured = m; });

    bot.queueUpdateBatch({makeUpdate(1091, kAllowedFromId, 0, "/setgmtoffset -5", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1091, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(-300, captured); // -5 hours * 60 = -300 minutes
}

// RFC-0175: /setgmtoffsetmin command — fn receives minutes directly
void test_TelegramPoller_setgmtoffsetmin_calls_fn_with_minutes()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    int captured = 999;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setGmtOffsetFn([&captured](int m) { captured = m; });

    bot.queueUpdateBatch({makeUpdate(1097, kAllowedFromId, 0, "/setgmtoffsetmin 330", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1097, poller.lastUpdateId());
    TEST_ASSERT_EQUAL(330, captured); // 330 minutes = UTC+5:30
}

// RFC-0170: /loginfo command
void test_TelegramPoller_loginfo_shows_count_and_capacity()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsDebugLog log;
    SmsDebugLog::Entry e;
    e.sender = "+8613800001111";
    e.outcome = "fwd OK";
    log.push(e);

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1092, kAllowedFromId, 0, "/loginfo", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1092, poller.lastUpdateId());
    auto msgs = bot.sentMessages();
    TEST_ASSERT_TRUE(msgs.size() > 0);
    // Should show "1/20 entries" and have the newest entry
    TEST_ASSERT_TRUE(msgs.back().indexOf("1/20") >= 0);
}

// RFC-0171: /smsrate command
void test_TelegramPoller_smsrate_replies_with_rate_info()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    SmsDebugLog log;
    // Push a fwd entry with a recent unix timestamp (won't show in rate since
    // time(nullptr) in native test returns near 0, but command should not crash)
    SmsDebugLog::Entry e;
    e.unixTimestamp = 1000;
    e.outcome = "fwd OK";
    log.push(e);

    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setDebugLog(&log);

    bot.queueUpdateBatch({makeUpdate(1093, kAllowedFromId, 0, "/smsrate", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1093, poller.lastUpdateId());
    auto msgs = bot.sentMessages();
    TEST_ASSERT_TRUE(msgs.size() > 0);
    // Reply should mention "1h" and "24h"
    TEST_ASSERT_TRUE(msgs.back().indexOf("1h") >= 0);
    TEST_ASSERT_TRUE(msgs.back().indexOf("24h") >= 0);
}

// RFC-0172: /setfwdtag command
void test_TelegramPoller_setfwdtag_calls_fn()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    String captured;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setFwdTagFn([&captured](const String &tag) { captured = tag; });

    bot.queueUpdateBatch({makeUpdate(1094, kAllowedFromId, 0, "/setfwdtag [Home]", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1094, poller.lastUpdateId());
    TEST_ASSERT_EQUAL_STRING("[Home]", captured.c_str());
}

// RFC-0173: /callstatus command
void test_TelegramPoller_callstatus_calls_fn_and_replies()
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
    poller.setCallStatusFn([]() -> String {
        return "Call notify: ON | Calls: 3";
    });

    bot.queueUpdateBatch({makeUpdate(1095, kAllowedFromId, 0, "/callstatus", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1095, poller.lastUpdateId());
    auto msgs = bot.sentMessages();
    TEST_ASSERT_TRUE(msgs.size() > 0);
    TEST_ASSERT_TRUE(msgs.back().indexOf("Calls: 3") >= 0);
}

// RFC-0174: /smshandlerinfo command
void test_TelegramPoller_smshandlerinfo_calls_fn_and_replies()
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
    poller.setSmsHandlerInfoFn([]() -> String {
        return "forwarding=ON blk=0 dup=0";
    });

    bot.queueUpdateBatch({makeUpdate(1096, kAllowedFromId, 0, "/smshandlerinfo", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1096, poller.lastUpdateId());
    auto msgs = bot.sentMessages();
    TEST_ASSERT_TRUE(msgs.size() > 0);
    TEST_ASSERT_TRUE(msgs.back().indexOf("forwarding=ON") >= 0);
}

// RFC-0181: /fwdtest calls fwdTestFn and sends result.
void test_TelegramPoller_fwdtest_calls_fn_and_sends_preview()
{
    FakeModem modem;
    FakeBotClient bot;
    FakePersist persist;
    SmsSender sender(modem);
    ReplyTargetMap rtm(persist);
    rtm.load();

    bool called = false;
    ClockFixture clk;
    TelegramPoller poller(bot, sender, rtm, persist,
                          [&]() -> uint32_t { return clk.nowMs; },
                          allowedAuth);
    poller.begin();
    poller.setFwdTestFn([&called]() -> String {
        called = true;
        return String("+10000000000 | 2026-04-10T12:00:00+08:00\n-----\nTest message");
    });

    bot.queueUpdateBatch({makeUpdate(1103, kAllowedFromId, 0, "/fwdtest", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1103, poller.lastUpdateId());
    TEST_ASSERT_TRUE(called);
    bool sawPreview = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("Test message") >= 0) { sawPreview = true; break; }
    TEST_ASSERT_TRUE(sawPreview);
}

// RFC-0187: /testfmt <phone> <body> — calls fwdTestPhoneBodyFn with parsed args.
void test_TelegramPoller_testfmt_calls_fn_with_phone_and_body()
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

    String gotPhone, gotBody;
    poller.setFwdTestPhoneBodyFn([&](const String &p, const String &b) -> String {
        gotPhone = p;
        gotBody  = b;
        return String(p + " | test preview: " + b);
    });

    bot.queueUpdateBatch({makeUpdate(1108, kAllowedFromId, 0,
        "/testfmt +13800138000 Hello world!", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1108, poller.lastUpdateId());
    TEST_ASSERT_EQUAL_STRING("+13800138000", gotPhone.c_str());
    TEST_ASSERT_EQUAL_STRING("Hello world!", gotBody.c_str());
    bool sawPreview = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("preview") >= 0) { sawPreview = true; break; }
    TEST_ASSERT_TRUE(sawPreview);
}

// RFC-0187: /testfmt with no body → usage reply.
void test_TelegramPoller_testfmt_no_body_sends_usage()
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
    poller.setFwdTestPhoneBodyFn([](const String &, const String &) -> String {
        return String("preview");
    });

    bot.queueUpdateBatch({makeUpdate(1109, kAllowedFromId, 0, "/testfmt +1234", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1109, poller.lastUpdateId());
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("Usage") >= 0) { sawUsage = true; break; }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0184: /factoryreset (no confirm) → warning message, no clearNvsFn call.
void test_TelegramPoller_factoryreset_without_confirm_sends_warning()
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

    bool clearCalled = false;
    poller.setClearNvsFn([&clearCalled]() { clearCalled = true; });

    bot.queueUpdateBatch({makeUpdate(1104, kAllowedFromId, 0, "/factoryreset", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1104, poller.lastUpdateId());
    TEST_ASSERT_FALSE(clearCalled);
    bool sawWarning = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("confirm") >= 0) { sawWarning = true; break; }
    TEST_ASSERT_TRUE(sawWarning);
}

// RFC-0184: /factoryreset confirm → calls clearNvsFn then rebootFn.
void test_TelegramPoller_factoryreset_confirm_calls_clear_and_reboot()
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

    bool clearCalled = false;
    bool rebootCalled = false;
    poller.setClearNvsFn([&clearCalled]() { clearCalled = true; });
    poller.setRebootFn([&rebootCalled](int64_t) { rebootCalled = true; });

    bot.queueUpdateBatch({makeUpdate(1105, kAllowedFromId, 0, "/factoryreset confirm", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1105, poller.lastUpdateId());
    TEST_ASSERT_TRUE(clearCalled);
    TEST_ASSERT_TRUE(rebootCalled);
}

// RFC-0195: /clearschedule — cancels all scheduled SMS at once.
void test_TelegramPoller_clearschedule_clears_all()
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
                          allowedAuth);
    poller.begin();
    clk.nowMs = 1000;

    // Schedule two entries.
    bot.queueUpdateBatch({makeUpdate(4001, kAllowedFromId, 0, "/schedulesend 60 +1111 First", kAllowedFromId)});
    poller.tick();
    clk.nowMs += 4000;
    bot.queueUpdateBatch({makeUpdate(4002, kAllowedFromId, 0, "/schedulesend 60 +2222 Second", kAllowedFromId)});
    poller.tick();
    clk.nowMs += 4000;
    bot.clearMessages();

    // /clearschedule should clear both.
    bot.queueUpdateBatch({makeUpdate(4003, kAllowedFromId, 0, "/clearschedule", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(4003, poller.lastUpdateId());
    bool sawCleared = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("Cleared") >= 0 && m.indexOf("2") >= 0) { sawCleared = true; break; }
    TEST_ASSERT_TRUE(sawCleared);
}

// RFC-0193: /sendnow — fires all scheduled SMS immediately.
void test_TelegramPoller_sendnow_fires_scheduled()
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
                          allowedAuth);
    poller.begin();
    clk.nowMs = 1000; // first tick processed

    // Schedule a future SMS.
    bot.queueUpdateBatch({makeUpdate(3001, kAllowedFromId, 0, "/schedulesend 60 +1234 Hi", kAllowedFromId)});
    poller.tick();
    TEST_ASSERT_EQUAL(3001, poller.lastUpdateId());

    // Advance time past the poll interval (3000ms) but not far enough to fire the scheduled slot.
    clk.nowMs += 4000;
    bot.clearMessages();

    // /sendnow should trigger it.
    bot.queueUpdateBatch({makeUpdate(3002, kAllowedFromId, 0, "/sendnow", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(3002, poller.lastUpdateId());
    bool sawTriggered = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("Triggering") >= 0 || m.indexOf("1") >= 0) { sawTriggered = true; break; }
    TEST_ASSERT_TRUE(sawTriggered);
}

// RFC-0192: /pausefwd 30 → calls pauseFwdFn with 30*60000ms, reply contains "30".
void test_TelegramPoller_pausefwd_calls_fn()
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

    uint32_t capturedMs = 0;
    poller.setPauseFwdFn([&capturedMs](uint32_t ms) -> String {
        capturedMs = ms;
        return String("Forwarding paused for 30 min.");
    });

    bot.queueUpdateBatch({makeUpdate(2010, kAllowedFromId, 0, "/pausefwd 30", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(2010, poller.lastUpdateId());
    TEST_ASSERT_EQUAL_UINT32(30 * 60000UL, capturedMs);
    bool sawPause = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("paused") >= 0 || m.indexOf("30") >= 0) { sawPause = true; break; }
    TEST_ASSERT_TRUE(sawPause);
}

// RFC-0191: /testpdu <hex> → calls testPduFn with the hex argument.
void test_TelegramPoller_testpdu_calls_fn_and_replies()
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

    String capturedHex;
    poller.setTestPduFn([&capturedHex](const String &hex) -> String {
        capturedHex = hex;
        return String("decoded: ") + hex.substring(0, 4);
    });

    bot.queueUpdateBatch({makeUpdate(2001, kAllowedFromId, 0, "/testpdu DEADBEEF", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(2001, poller.lastUpdateId());
    TEST_ASSERT_EQUAL_STRING("DEADBEEF", capturedHex.c_str());
    bool sawReply = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("decoded") >= 0) { sawReply = true; break; }
    TEST_ASSERT_TRUE(sawReply);
}

// RFC-0191: /testpdu with no argument → usage message.
void test_TelegramPoller_testpdu_no_arg_sends_usage()
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

    poller.setTestPduFn([](const String &) -> String { return String("x"); });

    bot.queueUpdateBatch({makeUpdate(2002, kAllowedFromId, 0, "/testpdu", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(2002, poller.lastUpdateId());
    bool sawUsage = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("Usage") >= 0) { sawUsage = true; break; }
    TEST_ASSERT_TRUE(sawUsage);
}

// RFC-0190: /setsmsagefilter 24 → calls smsAgeFilterFn with 24.
void test_TelegramPoller_setsmsagefilter_calls_fn()
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

    int capturedHours = -1;
    poller.setSmsAgeFilterFn([&capturedHours](int h) { capturedHours = h; });

    bot.queueUpdateBatch({makeUpdate(1110, kAllowedFromId, 0, "/setsmsagefilter 24", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1110, poller.lastUpdateId());
    TEST_ASSERT_EQUAL_INT(24, capturedHours);
    bool sawOk = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("24h") >= 0) { sawOk = true; break; }
    TEST_ASSERT_TRUE(sawOk);
}

// RFC-0190: /setsmsagefilter 0 → replies "age filter disabled".
void test_TelegramPoller_setsmsagefilter_zero_disables()
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

    int capturedHours = -1;
    poller.setSmsAgeFilterFn([&capturedHours](int h) { capturedHours = h; });

    bot.queueUpdateBatch({makeUpdate(1111, kAllowedFromId, 0, "/setsmsagefilter 0", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1111, poller.lastUpdateId());
    TEST_ASSERT_EQUAL_INT(0, capturedHours);
    bool sawDisabled = false;
    for (const auto &m : bot.sentMessages())
        if (m.indexOf("disabled") >= 0) { sawDisabled = true; break; }
    TEST_ASSERT_TRUE(sawDisabled);
}

// RFC-0190: /setsmsagefilter 9999 → out of range, error reply.
void test_TelegramPoller_setsmsagefilter_out_of_range()
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

    int capturedHours = -1;
    poller.setSmsAgeFilterFn([&capturedHours](int h) { capturedHours = h; });

    bot.queueUpdateBatch({makeUpdate(1112, kAllowedFromId, 0, "/setsmsagefilter 9999", kAllowedFromId)});
    poller.tick();

    TEST_ASSERT_EQUAL(1112, poller.lastUpdateId());
    // fn not called on error
    TEST_ASSERT_EQUAL_INT(-1, capturedHours);
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
    // RFC-0118: /label and /setlabel commands
    RUN_TEST(test_TelegramPoller_label_replies_with_current_label);
    RUN_TEST(test_TelegramPoller_label_no_label_set_replies_placeholder);
    RUN_TEST(test_TelegramPoller_setlabel_calls_setter_and_confirms);
    RUN_TEST(test_TelegramPoller_setlabel_no_arg_sends_usage);
    RUN_TEST(test_TelegramPoller_setlabel_too_long_sends_error);
    // RFC-0117: /history command
    RUN_TEST(test_TelegramPoller_history_filters_debug_log);
    RUN_TEST(test_TelegramPoller_history_no_arg_sends_usage);
    RUN_TEST(test_TelegramPoller_history_no_log_replies_not_configured);
    // RFC-0112: /reboot command
    RUN_TEST(test_TelegramPoller_reboot_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_reboot_not_configured_replies);
    // RFC-0119: /ping enhanced summary
    RUN_TEST(test_TelegramPoller_ping_uses_summary_fn);
    RUN_TEST(test_TelegramPoller_ping_fallback_without_fn);
    // RFC-0120: /uptime command
    RUN_TEST(test_TelegramPoller_uptime_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_uptime_not_configured_replies);
    // RFC-0121: /network command
    RUN_TEST(test_TelegramPoller_network_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_network_not_configured_replies);
    // RFC-0122: /logs command
    RUN_TEST(test_TelegramPoller_logs_returns_debug_log_entries);
    RUN_TEST(test_TelegramPoller_logs_respects_n_arg);
    RUN_TEST(test_TelegramPoller_logs_not_configured_replies);
    // RFC-0123: /boot command
    RUN_TEST(test_TelegramPoller_boot_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_boot_not_configured_replies);
    // RFC-0124: /count command
    RUN_TEST(test_TelegramPoller_count_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_count_not_configured_replies);
    // RFC-0125: /me command
    RUN_TEST(test_TelegramPoller_me_replies_with_ids);
    RUN_TEST(test_TelegramPoller_me_works_for_unauthorized_user);
    // RFC-0126: /ip command
    RUN_TEST(test_TelegramPoller_ip_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_ip_not_configured_replies);
    // RFC-0127: /smsslots command
    RUN_TEST(test_TelegramPoller_smsslots_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_smsslots_not_configured_replies);
    // RFC-0128: /lifetime command
    RUN_TEST(test_TelegramPoller_lifetime_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_lifetime_not_configured_replies);
    // RFC-0129: /announce command
    RUN_TEST(test_TelegramPoller_announce_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_announce_no_arg_sends_usage);
    RUN_TEST(test_TelegramPoller_announce_not_configured_replies);
    // RFC-0130: /digest command
    RUN_TEST(test_TelegramPoller_digest_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_digest_not_configured_replies);
    // RFC-0131: /note and /setnote commands
    RUN_TEST(test_TelegramPoller_note_replies_with_note);
    RUN_TEST(test_TelegramPoller_note_empty_replies_placeholder);
    RUN_TEST(test_TelegramPoller_setnote_calls_setter_and_confirms);
    // RFC-0132: /exportaliases command
    RUN_TEST(test_TelegramPoller_exportaliases_replies_with_csv);
    RUN_TEST(test_TelegramPoller_exportaliases_empty_store_replies_placeholder);
    // RFC-0133: /shortcuts command
    RUN_TEST(test_TelegramPoller_shortcuts_replies_with_quick_ref);
    // RFC-0134: /clearaliases command
    RUN_TEST(test_TelegramPoller_clearaliases_removes_all);
    RUN_TEST(test_TelegramPoller_clearaliases_empty_store_replies_placeholder);
    // RFC-0186: /importaliases command
    RUN_TEST(test_TelegramPoller_importaliases_valid_lines_imported);
    RUN_TEST(test_TelegramPoller_importaliases_skips_invalid_lines);
    // RFC-0136: /cancelnum command
    RUN_TEST(test_TelegramPoller_cancelnum_removes_matching_entries);
    RUN_TEST(test_TelegramPoller_cancelnum_no_match_replies_placeholder);
    RUN_TEST(test_TelegramPoller_cancelnum_no_arg_sends_usage);
    // RFC-0188: /schedulesend, /schedqueue, /cancelsched
    RUN_TEST(test_TelegramPoller_schedulesend_fires_after_delay);
    RUN_TEST(test_TelegramPoller_schedqueue_lists_pending);
    RUN_TEST(test_TelegramPoller_cancelsched_clears_slot);
    // RFC-0137: /setinterval command
    RUN_TEST(test_TelegramPoller_setinterval_calls_fn);
    RUN_TEST(test_TelegramPoller_setinterval_zero_disables);
    RUN_TEST(test_TelegramPoller_setinterval_too_large_sends_error);
    // RFC-0177: /hbnow command
    RUN_TEST(test_TelegramPoller_hbnow_calls_fn_and_replies_success);
    RUN_TEST(test_TelegramPoller_hbnow_replies_disabled_when_fn_returns_false);
    // RFC-0152: /resetwatermark
    RUN_TEST(test_TelegramPoller_resetwatermark_resets_to_zero);
    // RFC-0153: /setforward command
    RUN_TEST(test_TelegramPoller_setforward_off_calls_fn_false);
    RUN_TEST(test_TelegramPoller_setforward_on_calls_fn_true);
    // RFC-0151: auto-reply commands
    RUN_TEST(test_TelegramPoller_getautoreply_shows_text);
    RUN_TEST(test_TelegramPoller_setautoreply_calls_setter);
    RUN_TEST(test_TelegramPoller_clearautoreply_calls_setter_empty);
    // RFC-0148: /sweepsim command
    RUN_TEST(test_TelegramPoller_sweepsim_calls_fn_and_reports);
    RUN_TEST(test_TelegramPoller_sweepsim_zero_sms_replies_placeholder);
    // RFC-0149: /health command
    RUN_TEST(test_TelegramPoller_health_calls_fn);
    // RFC-0146: /forwardsim command
    RUN_TEST(test_TelegramPoller_forwardsim_calls_fn_with_index);
    RUN_TEST(test_TelegramPoller_forwardsim_no_arg_sends_usage);
    // RFC-0147: /setpollinterval command
    RUN_TEST(test_TelegramPoller_setpollinterval_updates_interval);
    RUN_TEST(test_TelegramPoller_setpollinterval_zero_sends_error);
    // RFC-0144: /setdedup command
    RUN_TEST(test_TelegramPoller_setdedup_calls_fn);
    RUN_TEST(test_TelegramPoller_setdedup_zero_disables);
    // RFC-0145: /cleardedup command
    RUN_TEST(test_TelegramPoller_cleardedup_calls_fn);
    // RFC-0142: /setconcatttl command
    RUN_TEST(test_TelegramPoller_setconcatttl_calls_fn);
    RUN_TEST(test_TelegramPoller_setconcatttl_too_small_sends_error);
    // RFC-0143: /modeminfo command
    RUN_TEST(test_TelegramPoller_modeminfo_calls_fn);
    RUN_TEST(test_TelegramPoller_modeminfo_not_configured_replies_placeholder);
    // RFC-0140: /simlist command
    RUN_TEST(test_TelegramPoller_simlist_calls_fn);
    RUN_TEST(test_TelegramPoller_simlist_not_configured_replies_placeholder);
    // RFC-0141: /simread command
    RUN_TEST(test_TelegramPoller_simread_calls_fn_with_index);
    RUN_TEST(test_TelegramPoller_simread_no_arg_sends_usage);
    RUN_TEST(test_TelegramPoller_simread_invalid_index_sends_error);
    // RFC-0138: /setmaxfail command
    RUN_TEST(test_TelegramPoller_setmaxfail_calls_fn);
    RUN_TEST(test_TelegramPoller_setmaxfail_zero_disables);
    RUN_TEST(test_TelegramPoller_setmaxfail_too_large_sends_error);
    // RFC-0139: /flushsim command
    RUN_TEST(test_TelegramPoller_flushsim_calls_fn);
    RUN_TEST(test_TelegramPoller_flushsim_without_yes_sends_usage);
    // RFC-0154: /logstats command
    RUN_TEST(test_TelegramPoller_logstats_returns_summary);
    RUN_TEST(test_TelegramPoller_logstats_no_log_replies_placeholder);
    // RFC-0155: /logsoutcome command
    RUN_TEST(test_TelegramPoller_logsoutcome_returns_matching_entries);
    RUN_TEST(test_TelegramPoller_logsoutcome_no_match_replies_placeholder);
    RUN_TEST(test_TelegramPoller_logsoutcome_no_arg_sends_usage);
    // RFC-0156: /simstatus command
    RUN_TEST(test_TelegramPoller_simstatus_calls_fn);
    RUN_TEST(test_TelegramPoller_simstatus_not_configured_replies_placeholder);
    // RFC-0157: /topn command
    RUN_TEST(test_TelegramPoller_topn_returns_top_senders);
    RUN_TEST(test_TelegramPoller_topn_no_log_replies_placeholder);
    // RFC-0158: /wifiscan command
    RUN_TEST(test_TelegramPoller_wifiscan_calls_fn);
    RUN_TEST(test_TelegramPoller_wifiscan_not_configured_replies_placeholder);
    // RFC-0159: /logsince command
    RUN_TEST(test_TelegramPoller_logsince_no_arg_sends_usage);
    RUN_TEST(test_TelegramPoller_logsince_invalid_hours_sends_error);
    // RFC-0179: /logcsv command
    RUN_TEST(test_TelegramPoller_logcsv_returns_csv_with_header);
    // RFC-0178: /logdate command
    RUN_TEST(test_TelegramPoller_logdate_valid_date_calls_debug_log);
    RUN_TEST(test_TelegramPoller_logdate_invalid_format_sends_error);
    // RFC-0160: /setmaxparts command
    RUN_TEST(test_TelegramPoller_setmaxparts_calls_fn);
    RUN_TEST(test_TelegramPoller_setmaxparts_out_of_range_sends_error);
    // RFC-0161: /smscount command
    RUN_TEST(test_TelegramPoller_smscount_calls_fn);
    RUN_TEST(test_TelegramPoller_smscount_not_configured_replies_placeholder);
    // RFC-0162: /setblockmode command
    RUN_TEST(test_TelegramPoller_setblockmode_off_calls_fn_false);
    RUN_TEST(test_TelegramPoller_setblockmode_on_calls_fn_true);
    // RFC-0163: /blockcheck command
    RUN_TEST(test_TelegramPoller_blockcheck_calls_fn);
    RUN_TEST(test_TelegramPoller_blockcheck_no_arg_sends_usage);
    // RFC-0164: /setcallnotify command
    RUN_TEST(test_TelegramPoller_setcallnotify_off_calls_fn_false);
    RUN_TEST(test_TelegramPoller_setcallnotify_on_calls_fn_true);
    // RFC-0165: /setcalldedup command
    RUN_TEST(test_TelegramPoller_setcalldedup_calls_fn_with_ms);
    RUN_TEST(test_TelegramPoller_setcalldedup_out_of_range_replies_error);
    // RFC-0166: /setunknowndeadline command
    RUN_TEST(test_TelegramPoller_setunknowndeadline_calls_fn_with_ms);
    RUN_TEST(test_TelegramPoller_setunknowndeadline_out_of_range_replies_error);
    // RFC-0167: /settings command
    RUN_TEST(test_TelegramPoller_settings_calls_fn_and_replies);
    // RFC-0168: /nvsinfo command
    RUN_TEST(test_TelegramPoller_nvsinfo_calls_fn_and_replies);
    // RFC-0169/0175: /setgmtoffset and /setgmtoffsetmin commands
    RUN_TEST(test_TelegramPoller_setgmtoffset_calls_fn_with_hours);
    RUN_TEST(test_TelegramPoller_setgmtoffsetmin_calls_fn_with_minutes);
    // RFC-0170: /loginfo command
    RUN_TEST(test_TelegramPoller_loginfo_shows_count_and_capacity);
    // RFC-0171: /smsrate command
    RUN_TEST(test_TelegramPoller_smsrate_replies_with_rate_info);
    // RFC-0172: /setfwdtag command
    RUN_TEST(test_TelegramPoller_setfwdtag_calls_fn);
    // RFC-0173: /callstatus command
    RUN_TEST(test_TelegramPoller_callstatus_calls_fn_and_replies);
    // RFC-0174: /smshandlerinfo command
    RUN_TEST(test_TelegramPoller_smshandlerinfo_calls_fn_and_replies);
    // RFC-0181: /fwdtest command
    RUN_TEST(test_TelegramPoller_fwdtest_calls_fn_and_sends_preview);
    // RFC-0187: /testfmt command
    RUN_TEST(test_TelegramPoller_testfmt_calls_fn_with_phone_and_body);
    RUN_TEST(test_TelegramPoller_testfmt_no_body_sends_usage);
    // RFC-0184: /factoryreset command
    RUN_TEST(test_TelegramPoller_factoryreset_without_confirm_sends_warning);
    RUN_TEST(test_TelegramPoller_factoryreset_confirm_calls_clear_and_reboot);
    // RFC-0111: outbound dedup
    RUN_TEST(test_TelegramPoller_send_duplicate_gets_already_queued_error);
    // RFC-0190: /setsmsagefilter command
    RUN_TEST(test_TelegramPoller_setsmsagefilter_calls_fn);
    RUN_TEST(test_TelegramPoller_setsmsagefilter_zero_disables);
    RUN_TEST(test_TelegramPoller_setsmsagefilter_out_of_range);
    // RFC-0191: /testpdu command
    RUN_TEST(test_TelegramPoller_testpdu_calls_fn_and_replies);
    RUN_TEST(test_TelegramPoller_testpdu_no_arg_sends_usage);
    // RFC-0192: /pausefwd command
    RUN_TEST(test_TelegramPoller_pausefwd_calls_fn);
    // RFC-0195: /clearschedule command
    RUN_TEST(test_TelegramPoller_clearschedule_clears_all);
    // RFC-0193: /sendnow command
    RUN_TEST(test_TelegramPoller_sendnow_fires_scheduled);
}
