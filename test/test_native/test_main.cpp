// Entry point for the native (host) test binary. PlatformIO's Unity
// runner calls main(), which in turn calls Unity's UNITY_BEGIN/END
// bracket around the RUN_TEST blocks from each test_*.cpp.

#include <unity.h>

void run_sms_codec_tests();
void run_sms_handler_tests();
void run_call_handler_tests();
void run_sms_pdu_tests();
void run_sms_handler_pdu_tests();
void run_reply_target_map_tests();
void run_sms_sender_tests();
void run_telegram_poller_tests();
void run_sms_pdu_encode_tests();
void run_delivery_report_tests();
void run_allow_list_tests();
void run_sms_block_list_tests();
void run_user_management_tests();
void run_sms_debug_log_tests();

void setUp() {}
void tearDown() {}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    run_sms_codec_tests();
    run_sms_handler_tests();
    run_call_handler_tests();
    run_sms_pdu_tests();
    run_sms_handler_pdu_tests();
    run_reply_target_map_tests();
    run_sms_sender_tests();
    run_telegram_poller_tests();
    run_sms_pdu_encode_tests();
    run_delivery_report_tests();
    run_allow_list_tests();
    run_sms_block_list_tests();
    run_user_management_tests();
    run_sms_debug_log_tests();
    return UNITY_END();
}
