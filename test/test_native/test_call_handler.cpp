// Unit tests for src/call_handler.{h,cpp} and the parseClipLine
// helper in src/sms_codec. Covers:
//   - +CLIP line parsing across firmware-version variants
//   - State machine: RING-then-CLIP, CLIP-then-RING, RING only
//   - Dedupe: rapid-fire URCs collapse to one notification
//   - Cooldown: two calls past the window produce two notifications
//   - Unknown-number deadline (no +CLIP ever arrives)
//   - Hangup fallback: callHangup() false -> raw AT+CHUP
//
// Runs on the host via [env:native] in platformio.ini.

#include <unity.h>
#include <Arduino.h>

#include "call_handler.h"
#include "sms_codec.h"
#include "fake_modem.h"
#include "fake_bot_client.h"

using sms_codec::parseClipLine;

// ---------- parseClipLine ----------

void test_parseClipLine_standard()
{
    // The shape we see most often on A76xx: quoted number, numeric
    // type, then three trailing fields (alpha, cli validity, presentation).
    String number;
    TEST_ASSERT_TRUE(parseClipLine(
        String("+CLIP: \"13800138000\",129,\"\",,\"\",0"), number));
    TEST_ASSERT_EQUAL_STRING("13800138000", number.c_str());
}

void test_parseClipLine_short_variant()
{
    // Some firmware builds emit only number + type.
    String number;
    TEST_ASSERT_TRUE(parseClipLine(
        String("+CLIP: \"13800138000\",129"), number));
    TEST_ASSERT_EQUAL_STRING("13800138000", number.c_str());
}

void test_parseClipLine_extra_fields()
{
    // More trailing fields than we care about — just ignore them.
    String number;
    TEST_ASSERT_TRUE(parseClipLine(
        String("+CLIP: \"+8613912345678\",145,\"\",,\"\",0,\"extra\",99"),
        number));
    TEST_ASSERT_EQUAL_STRING("+8613912345678", number.c_str());
}

void test_parseClipLine_empty_number_withheld()
{
    // CLI-withheld / anonymous — real modems emit an empty number field.
    String number;
    TEST_ASSERT_TRUE(parseClipLine(
        String("+CLIP: \"\",128,\"\",,\"\",0"), number));
    TEST_ASSERT_EQUAL_STRING("", number.c_str());
}

void test_parseClipLine_malformed_no_quotes()
{
    String number = String("untouched");
    TEST_ASSERT_FALSE(parseClipLine(
        String("+CLIP: 13800138000,129"), number));
}

void test_parseClipLine_malformed_no_comma_after_number()
{
    // Closing quote but no field separator after it — reject as garbage.
    String number = String("untouched");
    TEST_ASSERT_FALSE(parseClipLine(
        String("+CLIP: \"13800138000\""), number));
}

void test_parseClipLine_not_a_clip_line()
{
    String number = String("untouched");
    TEST_ASSERT_FALSE(parseClipLine(String("+CMTI: \"SM\",1"), number));
    TEST_ASSERT_FALSE(parseClipLine(String("OK"), number));
    TEST_ASSERT_FALSE(parseClipLine(String(""), number));
}

// ---------- CallHandler state machine fixtures ----------

// Virtual clock helper. Tests set `nowMs` directly and the CallHandler
// reads it through the ClockFn lambda the fixture injects.
struct ClockFixture
{
    uint32_t nowMs = 0;
};

// ---------- CallHandler: ordering ----------

void test_CallHandler_ring_then_clip_triggers_once()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    handler.onUrcLine(String("RING"));
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));

    TEST_ASSERT_EQUAL(1, (int)bot.callCount());
    TEST_ASSERT_EQUAL(1, modem.callHangupCalls());
    TEST_ASSERT_TRUE(
        bot.sentMessages()[0].indexOf(String("+86 138-0013-8000")) >= 0);
    TEST_ASSERT_TRUE(
        bot.sentMessages()[0].indexOf(String("auto-rejected")) >= 0);
    TEST_ASSERT_EQUAL((int)CallHandler::State::Cooldown, (int)handler.state());
}

void test_CallHandler_clip_then_ring_triggers_once()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    // Reverse order: some firmwares emit +CLIP before the matching RING.
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));
    handler.onUrcLine(String("RING"));

    TEST_ASSERT_EQUAL(1, (int)bot.callCount());
    TEST_ASSERT_EQUAL(1, modem.callHangupCalls());
    TEST_ASSERT_TRUE(
        bot.sentMessages()[0].indexOf(String("+86 138-0013-8000")) >= 0);
}

// ---------- CallHandler: dedupe window ----------

void test_CallHandler_rapid_fire_urcs_dedupe_to_one_event()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    // Rapid-fire RING/+CLIP as they arrive during a real call. Only
    // ONE notification + ONE hangup should result.
    handler.onUrcLine(String("RING"));
    clk.nowMs += 100;
    handler.onUrcLine(String("RING"));
    clk.nowMs += 100;
    handler.onUrcLine(String("RING"));
    clk.nowMs += 100;
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));
    clk.nowMs += 100;
    handler.onUrcLine(String("RING"));
    clk.nowMs += 100;
    handler.onUrcLine(String("RING"));
    clk.nowMs += 100;
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));

    TEST_ASSERT_EQUAL(1, (int)bot.callCount());
    TEST_ASSERT_EQUAL(1, modem.callHangupCalls());
}

// ---------- CallHandler: two back-to-back calls ----------

void test_CallHandler_two_calls_past_cooldown_trigger_two_events()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    // First call.
    clk.nowMs = 1000;
    handler.onUrcLine(String("RING"));
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));
    TEST_ASSERT_EQUAL(1, (int)bot.callCount());

    // Advance past the 6s dedupe window. tick() should drop us back
    // to Idle so the next call is not suppressed.
    clk.nowMs = 1000 + CallHandler::kDedupeWindowMs + 1000; // 7s after first event
    handler.tick();
    TEST_ASSERT_EQUAL((int)CallHandler::State::Idle, (int)handler.state());

    // Second call — same number, different event.
    handler.onUrcLine(String("RING"));
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));

    TEST_ASSERT_EQUAL(2, (int)bot.callCount());
    TEST_ASSERT_EQUAL(2, modem.callHangupCalls());
}

// ---------- CallHandler: unknown-number deadline ----------

void test_CallHandler_ring_without_clip_posts_unknown_after_deadline()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    clk.nowMs = 500;
    handler.onUrcLine(String("RING"));
    // No +CLIP ever arrives. tick() before the deadline should NOT commit.
    clk.nowMs = 500 + 100;
    handler.tick();
    TEST_ASSERT_EQUAL(0, (int)bot.callCount());
    TEST_ASSERT_EQUAL(0, modem.callHangupCalls());

    // Advance past the unknown-number deadline. Now tick() should
    // commit the event with "Unknown" as the caller.
    clk.nowMs = 500 + CallHandler::kUnknownNumberDeadlineMs + 1;
    handler.tick();

    TEST_ASSERT_EQUAL(1, (int)bot.callCount());
    TEST_ASSERT_EQUAL(1, modem.callHangupCalls());
    TEST_ASSERT_TRUE(bot.sentMessages()[0].indexOf(String("Unknown")) >= 0);
}

// ---------- CallHandler: withheld caller (+CLIP with empty number) ----------

void test_CallHandler_withheld_caller_says_unknown()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    handler.onUrcLine(String("RING"));
    handler.onUrcLine(String("+CLIP: \"\",128,\"\",,\"\",0"));

    TEST_ASSERT_EQUAL(1, (int)bot.callCount());
    TEST_ASSERT_EQUAL(1, modem.callHangupCalls());
    TEST_ASSERT_TRUE(bot.sentMessages()[0].indexOf(String("Unknown")) >= 0);
}

// ---------- CallHandler: hangup fallback ----------

void test_CallHandler_callHangup_false_falls_back_to_chup()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    // TinyGSM callHangup() returns false; handler should then send
    // a raw AT+CHUP and expect OK.
    modem.setCallHangupDefault(false);
    modem.queueOkEmpty(); // CHUP response

    handler.onUrcLine(String("RING"));
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));

    TEST_ASSERT_EQUAL(1, modem.callHangupCalls());
    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(1, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CHUP", sent[0].c_str());
}

void test_CallHandler_callHangup_true_does_not_send_chup()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    // Default is true — no fallback should fire.
    handler.onUrcLine(String("RING"));
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));

    TEST_ASSERT_EQUAL(1, modem.callHangupCalls());
    TEST_ASSERT_EQUAL(0, (int)modem.sentCommands().size());
}

// ---------- CallHandler: RING prefix false positive ----------

void test_CallHandler_RINGING_is_not_a_RING()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    // Adversarial: "RINGING" starts with "RING" but is not a ring URC.
    // We reject on length != 4.
    handler.onUrcLine(String("RINGING"));
    TEST_ASSERT_EQUAL((int)CallHandler::State::Idle, (int)handler.state());
    TEST_ASSERT_EQUAL(0, (int)bot.callCount());
}

// ---------- CallHandler: bot send failure doesn't block hangup ----------

void test_CallHandler_bot_send_failure_still_hangs_up()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    bot.setDefault(false);

    handler.onUrcLine(String("RING"));
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));

    // Bot was called once (and failed), hangup still fired.
    TEST_ASSERT_EQUAL(1, (int)bot.callCount());
    TEST_ASSERT_EQUAL(1, modem.callHangupCalls());
}

// ---------- CallHandler: ignores other URCs ----------

void test_CallHandler_ignores_unrelated_urcs()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    handler.onUrcLine(String("+CMTI: \"SM\",3"));
    handler.onUrcLine(String("OK"));
    handler.onUrcLine(String("+CREG: 1"));

    TEST_ASSERT_EQUAL((int)CallHandler::State::Idle, (int)handler.state());
    TEST_ASSERT_EQUAL(0, (int)bot.callCount());
    TEST_ASSERT_EQUAL(0, modem.callHangupCalls());
}

// ---------- Unity plumbing ----------

// RFC-0100 / RFC-0108: onCallFn fires with caller number and message_id on commit.
void test_CallHandler_onCallFn_fires_with_number()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    String capturedNumber;
    int32_t capturedMsgId = -1;
    handler.setOnCallFn([&](const String &num, int32_t msgId) {
        capturedNumber = num;
        capturedMsgId = msgId;
    });

    handler.onUrcLine(String("RING"));
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));

    TEST_ASSERT_TRUE(capturedNumber.indexOf(String("138")) >= 0);
    // FakeBotClient sendMessageReturningId returns 1 for the first call.
    TEST_ASSERT_TRUE(capturedMsgId > 0);
}

// RFC-0100 / RFC-0108: onCallFn fires with empty string for unknown caller.
void test_CallHandler_onCallFn_empty_for_unknown()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    bool called = false;
    String capturedNumber = "SENTINEL";
    int32_t capturedMsgId = -1;
    handler.setOnCallFn([&](const String &num, int32_t msgId) {
        called = true;
        capturedNumber = num;
        capturedMsgId = msgId;
    });

    // RING only, no +CLIP — should commit after deadline with empty number.
    handler.onUrcLine(String("RING"));
    clk.nowMs += CallHandler::kUnknownNumberDeadlineMs + 1;
    handler.tick();

    TEST_ASSERT_TRUE(called);
    TEST_ASSERT_EQUAL(0, (int)capturedNumber.length()); // empty for unknown
    // Unknown callers still get a message_id from the notification post.
    TEST_ASSERT_TRUE(capturedMsgId > 0);
}

// RFC-0108: Verify that the message_id from the call notification can be used
// to route a reply back as SMS (integration check with a single handler).
void test_CallHandler_onCallFn_provides_message_id_for_reply_routing()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });

    String callbackPhone;
    int32_t callbackMsgId = 0;
    handler.setOnCallFn([&](const String &num, int32_t msgId) {
        callbackPhone = num;
        callbackMsgId = msgId;
    });

    handler.onUrcLine(String("RING"));
    handler.onUrcLine(String("+CLIP: \"13800138000\",129,\"\",,\"\",0"));

    // The message_id should be > 0 (FakeBotClient auto-increments).
    TEST_ASSERT_TRUE(callbackMsgId > 0);
    TEST_ASSERT_EQUAL_STRING("13800138000", callbackPhone.c_str());
}

// RFC-0164: when callNotifyEnabled_ is false, bot receives no message but
// hangup still fires.
void test_CallHandler_muted_skips_bot_but_still_hangs_up()
{
    FakeModem modem;
    FakeBotClient bot;
    ClockFixture clk;
    CallHandler handler(modem, bot, [&]() { return clk.nowMs; });
    handler.setCallNotifyEnabled(false);

    handler.onUrcLine(String("RING"));
    handler.onUrcLine(String("+CLIP: \"12345\",129,\"\",,\"\",0"));

    TEST_ASSERT_EQUAL(0u, bot.sentMessages().size());
    // Hangup should still have fired: fake modem records the AT+CHUP / callHangup call.
    // FakeModem::callHangup increments a counter.
    TEST_ASSERT_TRUE(modem.callHangupCalls() > 0);
}

void run_call_handler_tests()
{
    RUN_TEST(test_parseClipLine_standard);
    RUN_TEST(test_parseClipLine_short_variant);
    RUN_TEST(test_parseClipLine_extra_fields);
    RUN_TEST(test_parseClipLine_empty_number_withheld);
    RUN_TEST(test_parseClipLine_malformed_no_quotes);
    RUN_TEST(test_parseClipLine_malformed_no_comma_after_number);
    RUN_TEST(test_parseClipLine_not_a_clip_line);

    RUN_TEST(test_CallHandler_ring_then_clip_triggers_once);
    RUN_TEST(test_CallHandler_clip_then_ring_triggers_once);
    RUN_TEST(test_CallHandler_rapid_fire_urcs_dedupe_to_one_event);
    RUN_TEST(test_CallHandler_two_calls_past_cooldown_trigger_two_events);
    RUN_TEST(test_CallHandler_ring_without_clip_posts_unknown_after_deadline);
    RUN_TEST(test_CallHandler_withheld_caller_says_unknown);
    RUN_TEST(test_CallHandler_callHangup_false_falls_back_to_chup);
    RUN_TEST(test_CallHandler_callHangup_true_does_not_send_chup);
    RUN_TEST(test_CallHandler_RINGING_is_not_a_RING);
    RUN_TEST(test_CallHandler_bot_send_failure_still_hangs_up);
    RUN_TEST(test_CallHandler_ignores_unrelated_urcs);
    // RFC-0100 / RFC-0108: onCallFn callback
    RUN_TEST(test_CallHandler_onCallFn_fires_with_number);
    RUN_TEST(test_CallHandler_onCallFn_empty_for_unknown);
    RUN_TEST(test_CallHandler_onCallFn_provides_message_id_for_reply_routing);
    // RFC-0164: muted call handling
    RUN_TEST(test_CallHandler_muted_skips_bot_but_still_hangs_up);
}
