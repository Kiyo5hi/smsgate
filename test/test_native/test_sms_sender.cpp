// Unit tests for src/sms_sender.{h,cpp}.
//
// SmsSender now builds SMS-SUBMIT PDU(s) via sms_codec and sends them
// through IModem::sendPduSms. Tests verify:
//   - ASCII body builds a GSM-7 PDU and calls sendPduSms (not sendSMS)
//   - Unicode body builds a UCS-2 PDU
//   - Modem failure propagates
//   - Empty phone / body rejected
//   - GSM-7 body > 10*153 chars rejected (maxParts cap)
//   - UCS-2 body > 10*67 chars rejected (maxParts cap)
//   - Exactly 160 GSM-7 chars: 1 part, no UDH
//   - Exactly 70 UCS-2 chars: 1 part, no UDH
//   - 161 GSM-7 chars: 2 parts with UDH
//   - Partial failure: second part fails -> SmsSender returns false

#include <unity.h>
#include <Arduino.h>

#include "sms_sender.h"
#include "sms_codec.h"
#include "sms_debug_log.h"
#include "fake_modem.h"
#include "fake_persist.h"

void test_SmsSender_ascii_builds_gsm7_pdu()
{
    FakeModem modem;
    SmsSender sender(modem);

    TEST_ASSERT_TRUE(sender.send(String("+8613800138000"), String("hello")));

    // Must use sendPduSms, NOT the old text-mode sendSMS.
    TEST_ASSERT_EQUAL(0, (int)modem.smsSendCalls().size());
    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());

    // Verify the PDU is plausible: starts with "00" (default SCA),
    // then "01" (SMS-SUBMIT first octet), then "00" (TP-MR).
    const String &hex = modem.pduSendCalls()[0].pduHex;
    TEST_ASSERT_TRUE(hex.startsWith("000100"));

    // No +CMGF=0 restoration needed (we never left PDU mode).
    TEST_ASSERT_EQUAL(0, (int)modem.sentCommands().size());

    TEST_ASSERT_EQUAL(0, (int)sender.lastError().length());
}

void test_SmsSender_unicode_builds_ucs2_pdu()
{
    FakeModem modem;
    SmsSender sender(modem);

    // "你好" in UTF-8 = 0xE4BDA0 0xE5A5BD
    String body;
    body += (char)(unsigned char)0xE4;
    body += (char)(unsigned char)0xBD;
    body += (char)(unsigned char)0xA0;
    body += (char)(unsigned char)0xE5;
    body += (char)(unsigned char)0xA5;
    body += (char)(unsigned char)0xBD;

    TEST_ASSERT_TRUE(sender.send(String("+8613800138000"), body));

    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());

    // The PDU should contain TP-DCS = 0x08 (UCS-2). Let's verify
    // by decoding: SCA(1) + firstOctet(1) + MR(1) + DA(varies) + PID(1) + DCS.
    // For +8613800138000: DA = 0D 91 68310080300F0 -> 8 bytes
    // So DCS is at byte offset 1 + 1 + 1 + 8 + 1 = 12, hex offset 24.
    const String &hex = modem.pduSendCalls()[0].pduHex;
    // The DCS byte in the hex string: after SCA(00) + first(01) + MR(00)
    // + DA(0D 91 ...). Phone "+8613800138000" has 13 digits -> 7 BCD bytes.
    // DA = [0D] [91] [68 31 00 80 03 F0 00] wait, that's wrong...
    // Actually: 13 digits, (13+1)/2 = 7 BCD bytes.
    // DA total = 1 (len) + 1 (TOA) + 7 (BCD) = 9 bytes = 18 hex chars.
    // Offset of TP-PID: 2 (SCA) + 2 (first) + 2 (MR) + 18 (DA) = 24
    // Offset of TP-DCS: 24 + 2 = 26
    String dcs = hex.substring(26, 28);
    TEST_ASSERT_EQUAL_STRING("08", dcs.c_str());

    // UDL should be 4 (two UCS-2 code units = 4 bytes)
    String udl = hex.substring(28, 30);
    TEST_ASSERT_EQUAL_STRING("04", udl.c_str());

    // "你好" in UTF-16BE = 4F60 597D
    String ud = hex.substring(30, 38);
    TEST_ASSERT_EQUAL_STRING("4F60597D", ud.c_str());
}

void test_SmsSender_modem_failure_propagates()
{
    FakeModem modem;
    SmsSender sender(modem);
    modem.setPduSendDefault(-1); // -1 = failure

    TEST_ASSERT_FALSE(sender.send(String("+8613800138000"), String("hello")));

    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("modem rejected")) >= 0);
}

void test_SmsSender_empty_phone_bails()
{
    FakeModem modem;
    SmsSender sender(modem);

    TEST_ASSERT_FALSE(sender.send(String(""), String("hello")));
    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("empty")) >= 0);
}

void test_SmsSender_empty_body_bails()
{
    FakeModem modem;
    SmsSender sender(modem);

    TEST_ASSERT_FALSE(sender.send(String("+8613800138000"), String("")));
    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("empty")) >= 0);
}

void test_SmsSender_gsm7_too_long_bails()
{
    FakeModem modem;
    SmsSender sender(modem);

    // Build a body that exceeds 10 parts (10 * 153 = 1530 septets).
    // 1531 'x' characters exceeds the maxParts=10 cap.
    String body;
    for (int i = 0; i < 1531; ++i)
        body += 'x';
    TEST_ASSERT_FALSE(sender.send(String("+8613800138000"), body));
    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("too long")) >= 0);
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("1530")) >= 0);
}

void test_SmsSender_ucs2_too_long_bails()
{
    FakeModem modem;
    SmsSender sender(modem);

    // Build a string exceeding 10 UCS-2 parts (10 * 67 = 670 chars).
    // 671 Chinese characters exceeds the maxParts=10 cap.
    String body;
    for (int i = 0; i < 671; ++i)
    {
        body += (char)(unsigned char)0xE4;
        body += (char)(unsigned char)0xBD;
        body += (char)(unsigned char)0xA0; // 你 (U+4F60)
    }
    TEST_ASSERT_FALSE(sender.send(String("+8613800138000"), body));
    TEST_ASSERT_EQUAL(0, (int)modem.pduSendCalls().size());
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("too long")) >= 0);
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("670")) >= 0);
}

void test_SmsSender_exactly_160_gsm7_succeeds()
{
    FakeModem modem;
    SmsSender sender(modem);

    String body;
    for (int i = 0; i < 160; ++i)
        body += 'A';
    TEST_ASSERT_TRUE(sender.send(String("+8613800138000"), body));
    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());
}

void test_SmsSender_exactly_70_ucs2_succeeds()
{
    FakeModem modem;
    SmsSender sender(modem);

    // 70 Chinese characters = 140 UCS-2 bytes (exactly at limit)
    String body;
    for (int i = 0; i < 70; ++i)
    {
        body += (char)(unsigned char)0xE4;
        body += (char)(unsigned char)0xBD;
        body += (char)(unsigned char)0xA0;
    }
    TEST_ASSERT_TRUE(sender.send(String("+8613800138000"), body));
    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());
}

// 161 GSM-7 characters -> two sendPduSms calls
void test_SmsSender_161_gsm7_sends_two_parts()
{
    FakeModem modem;
    SmsSender sender(modem);

    String body;
    for (int i = 0; i < 161; ++i)
        body += 'A';
    TEST_ASSERT_TRUE(sender.send(String("+8613800138000"), body));

    // Must have called sendPduSms exactly twice.
    TEST_ASSERT_EQUAL(2, (int)modem.pduSendCalls().size());

    // Both PDUs must start with "00" (SCA) then "41" (SMS-SUBMIT | UDHI).
    TEST_ASSERT_TRUE(modem.pduSendCalls()[0].pduHex.startsWith("004100"));
    TEST_ASSERT_TRUE(modem.pduSendCalls()[1].pduHex.startsWith("004100"));
}

// Partial failure: second part rejected -> SmsSender returns false
void test_SmsSender_partial_failure_second_part()
{
    FakeModem modem;
    SmsSender sender(modem);

    // First call succeeds (MR=0), second fails (MR=-1).
    modem.queuePduSendResult(0);
    modem.queuePduSendResult(-1);

    String body;
    for (int i = 0; i < 161; ++i)
        body += 'B';
    TEST_ASSERT_FALSE(sender.send(String("+8613800138000"), body));

    // Both parts were attempted.
    TEST_ASSERT_EQUAL(2, (int)modem.pduSendCalls().size());
    // Error message identifies which part failed.
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("part 2")) >= 0);
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("of 2")) >= 0);
}

// ---- RFC-0012 queue tests ----

// Happy path: enqueue then drainQueue(0) delivers immediately.
void test_SmsSender_enqueue_drain_success()
{
    FakeModem modem;
    SmsSender sender(modem);

    bool failureCalled = false;
    TEST_ASSERT_TRUE(sender.enqueue(String("+1"), String("hello"),
                                    [&]() { failureCalled = true; }));
    TEST_ASSERT_EQUAL(1, sender.queueSize());

    sender.drainQueue(0);

    // Entry was removed on success.
    TEST_ASSERT_EQUAL(0, sender.queueSize());
    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());
    TEST_ASSERT_FALSE(failureCalled);
}

// Retry: first attempt fails, then succeeds after the backoff window.
void test_SmsSender_enqueue_retry_after_backoff()
{
    FakeModem modem;
    SmsSender sender(modem);

    // Fail once (MR=-1), then succeed (MR=0).
    modem.queuePduSendResult(-1);
    modem.queuePduSendResult(0);

    bool failureCalled = false;
    sender.enqueue(String("+1"), String("hello"),
                   [&]() { failureCalled = true; });

    // Attempt 1 at t=0 — fails; nextRetryMs should be 0 + kBackoffMs[1] = 2000.
    sender.drainQueue(0);
    TEST_ASSERT_EQUAL(1, sender.queueSize()); // still queued
    TEST_ASSERT_FALSE(failureCalled);

    // t=500: not yet due (backoff = 2000 ms).
    sender.drainQueue(500);
    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size()); // no new send

    // t=2001: due — attempt 2 succeeds.
    sender.drainQueue(2001);
    TEST_ASSERT_EQUAL(2, (int)modem.pduSendCalls().size());
    TEST_ASSERT_EQUAL(0, sender.queueSize());
    TEST_ASSERT_FALSE(failureCalled);
}

// After kMaxAttempts failures, onFinalFailure is called and entry removed.
void test_SmsSender_max_retries_calls_on_final_failure()
{
    FakeModem modem;
    SmsSender sender(modem);
    modem.setPduSendDefault(-1); // always fail

    bool failureCalled = false;
    sender.enqueue(String("+1"), String("hello"),
                   [&]() { failureCalled = true; });

    // Drive through all 5 attempts with advancing clock.
    // Delays: immediate (0), 2000, 4000, 8000, 16000 ms between attempts.
    uint32_t t = 0;
    sender.drainQueue(t); // attempt 1 fails; nextRetry = 2000
    TEST_ASSERT_EQUAL(1, sender.queueSize());
    TEST_ASSERT_FALSE(failureCalled);

    t += 2000;
    sender.drainQueue(t); // attempt 2 fails; nextRetry = t+4000
    TEST_ASSERT_EQUAL(1, sender.queueSize());

    t += 4000;
    sender.drainQueue(t); // attempt 3 fails; nextRetry = t+8000
    TEST_ASSERT_EQUAL(1, sender.queueSize());

    t += 8000;
    sender.drainQueue(t); // attempt 4 fails; nextRetry = t+16000
    TEST_ASSERT_EQUAL(1, sender.queueSize());

    t += 16000;
    sender.drainQueue(t); // attempt 5 fails -> max reached, entry dropped
    TEST_ASSERT_EQUAL(0, sender.queueSize());
    TEST_ASSERT_TRUE(failureCalled);

    // Total 5 send attempts issued.
    TEST_ASSERT_EQUAL(5, (int)modem.pduSendCalls().size());
}

// Queue full: 8 slots filled; 9th enqueue returns false and calls onFinalFailure.
void test_SmsSender_queue_full_rejects_new_entry()
{
    FakeModem modem;
    SmsSender sender(modem);
    modem.setPduSendDefault(-1); // keep entries in queue by always failing

    // Fill all 8 slots.
    for (int i = 0; i < SmsSender::kQueueSize; ++i)
    {
        TEST_ASSERT_TRUE(sender.enqueue(String("+") + String(i), String("x")));
    }
    TEST_ASSERT_EQUAL(SmsSender::kQueueSize, sender.queueSize());

    // 9th enqueue should fail immediately.
    bool failureCalled = false;
    bool result = sender.enqueue(String("+99"), String("overflow"),
                                 [&]() { failureCalled = true; });
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(failureCalled);
    // Queue still at 8.
    TEST_ASSERT_EQUAL(SmsSender::kQueueSize, sender.queueSize());
}

// One send per drainQueue call: two ready entries -> only one send attempt.
void test_SmsSender_drain_queue_one_send_per_call()
{
    FakeModem modem;
    SmsSender sender(modem);

    sender.enqueue(String("+1"), String("first"));
    sender.enqueue(String("+2"), String("second"));
    TEST_ASSERT_EQUAL(2, sender.queueSize());

    // Both entries are immediately due; only one should be sent per call.
    sender.drainQueue(0);
    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());
    TEST_ASSERT_EQUAL(1, sender.queueSize());

    sender.drainQueue(0);
    TEST_ASSERT_EQUAL(2, (int)modem.pduSendCalls().size());
    TEST_ASSERT_EQUAL(0, sender.queueSize());
}

// RFC-0032: onSuccess is called once on successful drain, then cleared.
void test_SmsSender_on_success_called_after_delivery()
{
    FakeModem modem;
    SmsSender sender(modem);

    int successCount = 0;
    TEST_ASSERT_TRUE(sender.enqueue(String("+1"), String("hello"),
                                    nullptr, // no failure cb
                                    [&]() { successCount++; }));
    TEST_ASSERT_EQUAL(1, sender.queueSize());

    sender.drainQueue(0);

    TEST_ASSERT_EQUAL(0, sender.queueSize());
    TEST_ASSERT_EQUAL(1, successCount); // fired exactly once
    TEST_ASSERT_EQUAL(1, (int)modem.pduSendCalls().size());
}

// RFC-0032: onSuccess is NOT called on final failure.
void test_SmsSender_on_success_not_called_on_failure()
{
    FakeModem modem;
    SmsSender sender(modem);
    modem.setPduSendDefault(-1); // always fail

    bool successCalled = false;
    bool failureCalled = false;
    sender.enqueue(String("+1"), String("hello"),
                   [&]() { failureCalled = true; },
                   [&]() { successCalled = true; });

    // Exhaust all retries.
    uint32_t t = 0;
    for (int i = 0; i < SmsSender::kMaxAttempts; ++i)
    {
        sender.drainQueue(t);
        int idx = (i + 1 < 5) ? (i + 1) : 4;
        t += SmsSender::kBackoffMs[idx] + 1;
    }

    TEST_ASSERT_EQUAL(0, sender.queueSize());
    TEST_ASSERT_TRUE(failureCalled);
    TEST_ASSERT_FALSE(successCalled); // must NOT fire on failure
}

// Nullptr onFinalFailure: exhaust retries without crashing.
void test_SmsSender_nullptr_on_final_failure_no_crash()
{
    FakeModem modem;
    SmsSender sender(modem);
    modem.setPduSendDefault(-1);

    // Pass nullptr — no crash expected on final failure.
    sender.enqueue(String("+1"), String("hello"), nullptr);

    uint32_t t = 0;
    for (int attempt = 0; attempt < SmsSender::kMaxAttempts; ++attempt)
    {
        sender.drainQueue(t);
        // Advance past the next backoff period.
        int idx = (attempt + 1 < 5) ? (attempt + 1) : 4;
        t += SmsSender::kBackoffMs[idx] + 1;
    }
    // Entry should be gone, no crash.
    TEST_ASSERT_EQUAL(0, sender.queueSize());
}

// RFC-0033: getQueueSnapshot returns empty when queue is empty.
void test_SmsSender_snapshot_empty_queue()
{
    FakeModem modem;
    SmsSender sender(modem);
    TEST_ASSERT_EQUAL(0, (int)sender.getQueueSnapshot().size());
}

// RFC-0033: getQueueSnapshot captures phone, bodyPreview, and attempts.
void test_SmsSender_snapshot_captures_entry()
{
    FakeModem modem;
    SmsSender sender(modem);
    modem.setPduSendDefault(-1); // keep in queue

    sender.enqueue(String("+1"), String("Hello world this is longer than twenty chars"));
    sender.drainQueue(0); // fail → attempts becomes 1

    auto snap = sender.getQueueSnapshot();
    TEST_ASSERT_EQUAL(1, (int)snap.size());
    TEST_ASSERT_EQUAL_STRING("+1", snap[0].phone.c_str());
    // bodyPreview is first 20 chars
    TEST_ASSERT_EQUAL_STRING("Hello world this is ", snap[0].bodyPreview.c_str());
    TEST_ASSERT_EQUAL(1, snap[0].attempts);
}

// RFC-0035: queue_full logs an "out:queue_full" entry to the debug log.
void test_SmsSender_queue_full_logs_to_debug_log()
{
    FakeModem modem;
    FakePersist persist;
    SmsDebugLog log;
    SmsSender sender(modem);
    sender.setDebugLog(&log);
    modem.setPduSendDefault(-1); // keep entries in queue

    // Fill all 8 slots.
    for (int i = 0; i < SmsSender::kQueueSize; ++i)
        sender.enqueue(String("+") + String(i), String("x"));
    TEST_ASSERT_EQUAL(0, (int)log.count()); // no log entries yet

    // 9th enqueue — queue full → should log.
    sender.enqueue(String("+99"), String("overflow body"));
    TEST_ASSERT_EQUAL(1, (int)log.count());
    TEST_ASSERT_TRUE(log.dump().indexOf(String("out:queue_full")) >= 0);
    TEST_ASSERT_TRUE(log.dump().indexOf(String("+99")) >= 0);
}

// RFC-0035: final failure after kMaxAttempts logs "out:fail:" entry.
void test_SmsSender_final_failure_logs_to_debug_log()
{
    FakeModem modem;
    SmsDebugLog log;
    SmsSender sender(modem);
    sender.setDebugLog(&log);
    modem.setPduSendDefault(-1); // always fail

    sender.enqueue(String("+1"), String("hello fail"));

    // Exhaust all retries.
    uint32_t t = 0;
    for (int i = 0; i < SmsSender::kMaxAttempts; ++i)
    {
        sender.drainQueue(t);
        int idx = (i + 1 < 5) ? (i + 1) : 4;
        t += SmsSender::kBackoffMs[idx] + 1;
    }

    TEST_ASSERT_EQUAL(0, sender.queueSize()); // entry removed
    TEST_ASSERT_EQUAL(1, (int)log.count());
    TEST_ASSERT_TRUE(log.dump().indexOf(String("out:fail:")) >= 0);
    TEST_ASSERT_TRUE(log.dump().indexOf(String("+1")) >= 0);
}

// RFC-0046: cancelQueueEntry removes the Nth occupied entry (1-indexed).
void test_SmsSender_cancel_removes_entry()
{
    FakeModem modem;
    SmsSender sender(modem);
    modem.setPduSendDefault(-1); // never succeeds → stays in queue
    sender.enqueue(String("+11"), String("msg1"));
    sender.enqueue(String("+22"), String("msg2"));
    TEST_ASSERT_EQUAL(2, sender.queueSize());
    bool ok = sender.cancelQueueEntry(1); // cancel first entry
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, sender.queueSize());
    auto snap = sender.getQueueSnapshot();
    TEST_ASSERT_EQUAL_STRING("+22", snap[0].phone.c_str()); // second entry remains
}

void test_SmsSender_cancel_out_of_range_returns_false()
{
    FakeModem modem;
    SmsSender sender(modem);
    modem.setPduSendDefault(-1);
    sender.enqueue(String("+11"), String("msg1"));
    TEST_ASSERT_FALSE(sender.cancelQueueEntry(0)); // 0 is never valid
    TEST_ASSERT_FALSE(sender.cancelQueueEntry(2)); // only 1 entry
    TEST_ASSERT_EQUAL(1, sender.queueSize()); // unchanged
}

// RFC-0087: resetRetryTimers resets all nextRetryMs to 0.
void test_SmsSender_resetRetryTimers_unblocks_entry()
{
    FakeModem modem;
    SmsSender sender(modem);
    modem.setPduSendDefault(-1); // fail once to trigger backoff

    sender.enqueue(String("+1"), String("hello"));
    sender.drainQueue(0); // attempt 1 → fails, sets nextRetryMs > 0

    // Without reset the entry would not be retried until backoff expires.
    // Reset timers; now it should be retried immediately.
    sender.resetRetryTimers();
    modem.setPduSendDefault(1); // next send succeeds
    bool successCalled = false;
    // We enqueued already; can't set callback retroactively, but we can
    // verify drainQueue runs the entry (queueSize decrements on success).
    sender.drainQueue(0); // should succeed and remove entry
    TEST_ASSERT_EQUAL(0, sender.queueSize());
}

// RFC-0086: successful drain logs "out:sent" to debug log.
void test_SmsSender_success_logs_to_debug_log()
{
    FakeModem modem;
    SmsDebugLog log;
    SmsSender sender(modem);
    sender.setDebugLog(&log);
    modem.setPduSendDefault(1); // always succeed

    sender.enqueue(String("+1"), String("hello success"));
    sender.drainQueue(0);

    TEST_ASSERT_EQUAL(0, sender.queueSize()); // entry removed
    TEST_ASSERT_EQUAL(1, (int)log.count());
    TEST_ASSERT_TRUE(log.dump().indexOf(String("out:sent")) >= 0);
}

void run_sms_sender_tests()
{
    RUN_TEST(test_SmsSender_ascii_builds_gsm7_pdu);
    RUN_TEST(test_SmsSender_unicode_builds_ucs2_pdu);
    RUN_TEST(test_SmsSender_modem_failure_propagates);
    RUN_TEST(test_SmsSender_empty_phone_bails);
    RUN_TEST(test_SmsSender_empty_body_bails);
    RUN_TEST(test_SmsSender_gsm7_too_long_bails);
    RUN_TEST(test_SmsSender_ucs2_too_long_bails);
    RUN_TEST(test_SmsSender_exactly_160_gsm7_succeeds);
    RUN_TEST(test_SmsSender_exactly_70_ucs2_succeeds);
    RUN_TEST(test_SmsSender_161_gsm7_sends_two_parts);
    RUN_TEST(test_SmsSender_partial_failure_second_part);
    RUN_TEST(test_SmsSender_enqueue_drain_success);
    RUN_TEST(test_SmsSender_enqueue_retry_after_backoff);
    RUN_TEST(test_SmsSender_max_retries_calls_on_final_failure);
    RUN_TEST(test_SmsSender_queue_full_rejects_new_entry);
    RUN_TEST(test_SmsSender_drain_queue_one_send_per_call);
    RUN_TEST(test_SmsSender_nullptr_on_final_failure_no_crash);
    RUN_TEST(test_SmsSender_on_success_called_after_delivery);
    RUN_TEST(test_SmsSender_on_success_not_called_on_failure);
    RUN_TEST(test_SmsSender_snapshot_empty_queue);
    RUN_TEST(test_SmsSender_snapshot_captures_entry);
    RUN_TEST(test_SmsSender_queue_full_logs_to_debug_log);
    RUN_TEST(test_SmsSender_final_failure_logs_to_debug_log);
    RUN_TEST(test_SmsSender_cancel_removes_entry);
    RUN_TEST(test_SmsSender_cancel_out_of_range_returns_false);
    // RFC-0087: resetRetryTimers
    RUN_TEST(test_SmsSender_resetRetryTimers_unblocks_entry);
    // RFC-0086: outbound success log
    RUN_TEST(test_SmsSender_success_logs_to_debug_log);
}
