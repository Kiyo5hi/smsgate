// Unit tests for src/sms_block_list.h (RFC-0018).
// parseBlockList() and isBlocked() are pure <string.h>-only functions —
// no Arduino or hardware dependencies — exercised directly on the host.

#include <unity.h>
#include <string.h>

#include "sms_block_list.h"

// ---- parseBlockList ----

void test_parseBlockList_null_csv_returns_zero()
{
    char out[5][kSmsBlockListMaxNumberLen + 1];
    int n = parseBlockList(nullptr, out, 5);
    TEST_ASSERT_EQUAL(0, n);
}

void test_parseBlockList_empty_csv_returns_zero()
{
    char out[5][kSmsBlockListMaxNumberLen + 1];
    int n = parseBlockList("", out, 5);
    TEST_ASSERT_EQUAL(0, n);
}

void test_parseBlockList_single_entry_no_whitespace()
{
    char out[5][kSmsBlockListMaxNumberLen + 1];
    int n = parseBlockList("10086", out, 5);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("10086", out[0]);
}

void test_parseBlockList_two_entries()
{
    char out[5][kSmsBlockListMaxNumberLen + 1];
    int n = parseBlockList("10086,10010", out, 5);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("10086", out[0]);
    TEST_ASSERT_EQUAL_STRING("10010", out[1]);
}

void test_parseBlockList_strips_leading_trailing_whitespace()
{
    char out[5][kSmsBlockListMaxNumberLen + 1];
    int n = parseBlockList("  10086  ,\t10010\t", out, 5);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("10086", out[0]);
    TEST_ASSERT_EQUAL_STRING("10010", out[1]);
}

void test_parseBlockList_skips_empty_tokens_adjacent_commas()
{
    char out[5][kSmsBlockListMaxNumberLen + 1];
    int n = parseBlockList("10086,,10010", out, 5);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("10086", out[0]);
    TEST_ASSERT_EQUAL_STRING("10010", out[1]);
}

void test_parseBlockList_trailing_comma_no_extra_entry()
{
    char out[5][kSmsBlockListMaxNumberLen + 1];
    int n = parseBlockList("10086,", out, 5);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("10086", out[0]);
}

void test_parseBlockList_token_longer_than_max_truncated()
{
    // 21 chars: longer than kSmsBlockListMaxNumberLen (20)
    const char *longToken = "123456789012345678901";
    char out[5][kSmsBlockListMaxNumberLen + 1];
    int n = parseBlockList(longToken, out, 5);
    TEST_ASSERT_EQUAL(1, n);
    // Entry is stored but truncated to exactly 20 chars
    TEST_ASSERT_EQUAL(kSmsBlockListMaxNumberLen, (int)strlen(out[0]));
    TEST_ASSERT_EQUAL_STRING("12345678901234567890", out[0]);
}

void test_parseBlockList_maxEntries_zero_returns_zero()
{
    char out[5][kSmsBlockListMaxNumberLen + 1];
    int n = parseBlockList("10086,10010", out, 0);
    TEST_ASSERT_EQUAL(0, n);
}

void test_parseBlockList_exactly_max_entries_no_overflow()
{
    // Build a CSV with maxEntries + 1 tokens; array only has maxEntries slots.
    // Use maxEntries = 3 to keep the test small.
    char out[3][kSmsBlockListMaxNumberLen + 1];
    int n = parseBlockList("A,B,C,D", out, 3);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_STRING("A", out[0]);
    TEST_ASSERT_EQUAL_STRING("B", out[1]);
    TEST_ASSERT_EQUAL_STRING("C", out[2]);
}

// ---- isBlocked ----

void test_isBlocked_null_number_returns_false()
{
    char list[2][kSmsBlockListMaxNumberLen + 1];
    strncpy(list[0], "10086", kSmsBlockListMaxNumberLen + 1);
    strncpy(list[1], "10010", kSmsBlockListMaxNumberLen + 1);
    TEST_ASSERT_FALSE(isBlocked(nullptr, list, 2));
}

void test_isBlocked_zero_count_returns_false()
{
    char list[2][kSmsBlockListMaxNumberLen + 1];
    strncpy(list[0], "10086", kSmsBlockListMaxNumberLen + 1);
    TEST_ASSERT_FALSE(isBlocked("10086", list, 0));
}

void test_isBlocked_null_list_returns_false()
{
    TEST_ASSERT_FALSE(isBlocked("10086", nullptr, 1));
}

void test_isBlocked_null_list_zero_count_returns_false()
{
    TEST_ASSERT_FALSE(isBlocked("10086", nullptr, 0));
}

void test_isBlocked_number_not_in_list_returns_false()
{
    char list[2][kSmsBlockListMaxNumberLen + 1];
    strncpy(list[0], "10010", kSmsBlockListMaxNumberLen + 1);
    strncpy(list[1], "+8610010", kSmsBlockListMaxNumberLen + 1);
    TEST_ASSERT_FALSE(isBlocked("10086", list, 2));
}

void test_isBlocked_number_in_list_returns_true()
{
    char list[2][kSmsBlockListMaxNumberLen + 1];
    strncpy(list[0], "10010", kSmsBlockListMaxNumberLen + 1);
    strncpy(list[1], "10086", kSmsBlockListMaxNumberLen + 1);
    TEST_ASSERT_TRUE(isBlocked("10086", list, 2));
}

void test_isBlocked_prefix_does_not_match()
{
    char list[1][kSmsBlockListMaxNumberLen + 1];
    strncpy(list[0], "10086", kSmsBlockListMaxNumberLen + 1);
    TEST_ASSERT_FALSE(isBlocked("1008", list, 1));
}

void test_isBlocked_suffix_does_not_match()
{
    char list[1][kSmsBlockListMaxNumberLen + 1];
    strncpy(list[0], "10086", kSmsBlockListMaxNumberLen + 1);
    TEST_ASSERT_FALSE(isBlocked("10086x", list, 1));
}

void test_isBlocked_e164_matches_e164_entry()
{
    char list[1][kSmsBlockListMaxNumberLen + 1];
    strncpy(list[0], "+8610086", kSmsBlockListMaxNumberLen + 1);
    TEST_ASSERT_TRUE(isBlocked("+8610086", list, 1));
}

void test_isBlocked_e164_does_not_match_national_entry()
{
    char list[1][kSmsBlockListMaxNumberLen + 1];
    strncpy(list[0], "10086", kSmsBlockListMaxNumberLen + 1);
    TEST_ASSERT_FALSE(isBlocked("+8610086", list, 1));
}

// ---- Unity plumbing ----

void run_sms_block_list_tests()
{
    RUN_TEST(test_parseBlockList_null_csv_returns_zero);
    RUN_TEST(test_parseBlockList_empty_csv_returns_zero);
    RUN_TEST(test_parseBlockList_single_entry_no_whitespace);
    RUN_TEST(test_parseBlockList_two_entries);
    RUN_TEST(test_parseBlockList_strips_leading_trailing_whitespace);
    RUN_TEST(test_parseBlockList_skips_empty_tokens_adjacent_commas);
    RUN_TEST(test_parseBlockList_trailing_comma_no_extra_entry);
    RUN_TEST(test_parseBlockList_token_longer_than_max_truncated);
    RUN_TEST(test_parseBlockList_maxEntries_zero_returns_zero);
    RUN_TEST(test_parseBlockList_exactly_max_entries_no_overflow);
    RUN_TEST(test_isBlocked_null_number_returns_false);
    RUN_TEST(test_isBlocked_zero_count_returns_false);
    RUN_TEST(test_isBlocked_null_list_returns_false);
    RUN_TEST(test_isBlocked_null_list_zero_count_returns_false);
    RUN_TEST(test_isBlocked_number_not_in_list_returns_false);
    RUN_TEST(test_isBlocked_number_in_list_returns_true);
    RUN_TEST(test_isBlocked_prefix_does_not_match);
    RUN_TEST(test_isBlocked_suffix_does_not_match);
    RUN_TEST(test_isBlocked_e164_matches_e164_entry);
    RUN_TEST(test_isBlocked_e164_does_not_match_national_entry);
}
