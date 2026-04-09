// Entry point for the native (host) test binary. PlatformIO's Unity
// runner calls main(), which in turn calls Unity's UNITY_BEGIN/END
// bracket around the RUN_TEST blocks from each test_*.cpp.

#include <unity.h>

void run_sms_codec_tests();
void run_sms_handler_tests();
void run_sms_pdu_tests();
void run_sms_handler_pdu_tests();

void setUp() {}
void tearDown() {}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    run_sms_codec_tests();
    run_sms_handler_tests();
    run_sms_pdu_tests();
    run_sms_handler_pdu_tests();
    return UNITY_END();
}
