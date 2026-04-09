// Unit tests for src/sms_sender.{h,cpp}.

#include <unity.h>
#include <Arduino.h>

#include "sms_sender.h"
#include "fake_modem.h"

void test_SmsSender_ascii_happy_path_restores_pdu_mode()
{
    FakeModem modem;
    SmsSender sender(modem);
    // Queue OK for the +CMGF=0 we expect after the send.
    modem.queueOkEmpty();

    TEST_ASSERT_TRUE(sender.send(String("+8613800138000"), String("hello world")));

    // FakeModem records sendSMS calls separately from sendAT.
    TEST_ASSERT_EQUAL(1, (int)modem.smsSendCalls().size());
    TEST_ASSERT_EQUAL_STRING("+8613800138000", modem.smsSendCalls()[0].number.c_str());
    TEST_ASSERT_EQUAL_STRING("hello world", modem.smsSendCalls()[0].text.c_str());

    // After the send, we expect a +CMGF=0 to restore PDU mode.
    const auto &sent = modem.sentCommands();
    TEST_ASSERT_EQUAL(1, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("+CMGF=0", sent[0].c_str());

    TEST_ASSERT_EQUAL(0, (int)sender.lastError().length());
}

void test_SmsSender_modem_failure_still_restores_pdu_mode()
{
    FakeModem modem;
    SmsSender sender(modem);
    modem.setSmsSendDefault(false);
    // Queue OK for the +CMGF=0 anyway.
    modem.queueOkEmpty();

    TEST_ASSERT_FALSE(sender.send(String("+8613800138000"), String("hello")));

    // PDU mode restoration must run regardless.
    TEST_ASSERT_EQUAL(1, (int)modem.sentCommands().size());
    TEST_ASSERT_EQUAL_STRING("+CMGF=0", modem.sentCommands()[0].c_str());

    TEST_ASSERT_TRUE(sender.lastError().length() > 0);
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("modem rejected")) >= 0);
}

void test_SmsSender_non_ascii_body_bails_without_calling_modem()
{
    FakeModem modem;
    SmsSender sender(modem);

    // "你好" in UTF-8 = 0xE4 0xBD 0xA0 0xE5 0xA5 0xBD
    String body;
    body += (char)0xE4;
    body += (char)0xBD;
    body += (char)0xA0;
    body += (char)0xE5;
    body += (char)0xA5;
    body += (char)0xBD;

    TEST_ASSERT_FALSE(sender.send(String("+8613800138000"), body));

    // No sendSMS call, no AT traffic.
    TEST_ASSERT_EQUAL(0, (int)modem.smsSendCalls().size());
    TEST_ASSERT_EQUAL(0, (int)modem.sentCommands().size());

    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("non-ASCII")) >= 0);
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("RFC-0002")) >= 0);
}

void test_SmsSender_empty_phone_bails()
{
    FakeModem modem;
    SmsSender sender(modem);

    TEST_ASSERT_FALSE(sender.send(String(""), String("hello")));
    TEST_ASSERT_EQUAL(0, (int)modem.smsSendCalls().size());
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("empty")) >= 0);
}

void test_SmsSender_too_long_body_bails()
{
    FakeModem modem;
    SmsSender sender(modem);

    String body;
    for (int i = 0; i < 161; ++i)
        body += 'x';
    TEST_ASSERT_FALSE(sender.send(String("+8613800138000"), body));
    TEST_ASSERT_EQUAL(0, (int)modem.smsSendCalls().size());
    TEST_ASSERT_TRUE(sender.lastError().indexOf(String("too long")) >= 0);
}

void run_sms_sender_tests()
{
    RUN_TEST(test_SmsSender_ascii_happy_path_restores_pdu_mode);
    RUN_TEST(test_SmsSender_modem_failure_still_restores_pdu_mode);
    RUN_TEST(test_SmsSender_non_ascii_body_bails_without_calling_modem);
    RUN_TEST(test_SmsSender_empty_phone_bails);
    RUN_TEST(test_SmsSender_too_long_body_bails);
}
