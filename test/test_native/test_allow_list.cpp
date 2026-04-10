// Unit tests for RFC-0014: multi-user allow list.
//
// Covers:
//   parseAllowedIds() — CSV parsing, whitespace trimming, truncation
//   AuthFn scan — single entry match/miss, multi-entry, fromId==0, empty list

#include <unity.h>
#include <Arduino.h>
#include "allow_list.h"

// ------------------------------------------------------------------ //
// parseAllowedIds tests
// ------------------------------------------------------------------ //

void test_allow_list_single_entry()
{
    int64_t out[10] = {};
    int n = parseAllowedIds("123456789", out, 10);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(123456789LL, out[0]);
}

void test_allow_list_multiple_entries()
{
    int64_t out[10] = {};
    int n = parseAllowedIds("111,222,333", out, 10);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL(111LL, out[0]);
    TEST_ASSERT_EQUAL(222LL, out[1]);
    TEST_ASSERT_EQUAL(333LL, out[2]);
}

void test_allow_list_leading_trailing_whitespace()
{
    int64_t out[10] = {};
    int n = parseAllowedIds("  111 , 222  ,  333  ", out, 10);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL(111LL, out[0]);
    TEST_ASSERT_EQUAL(222LL, out[1]);
    TEST_ASSERT_EQUAL(333LL, out[2]);
}

void test_allow_list_truncates_at_max()
{
    // maxIds=2, but CSV has 3 entries — only first 2 should be stored.
    int64_t out[3] = {};
    int n = parseAllowedIds("11,22,33", out, 2);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(11LL, out[0]);
    TEST_ASSERT_EQUAL(22LL, out[1]);
    // Third slot untouched.
    TEST_ASSERT_EQUAL(0LL, out[2]);
}

void test_allow_list_skips_zero_tokens()
{
    // "0" parses to 0 and is skipped; empty token between commas also skipped.
    int64_t out[10] = {};
    int n = parseAllowedIds("0,999,,888", out, 10);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(999LL, out[0]);
    TEST_ASSERT_EQUAL(888LL, out[1]);
}

void test_allow_list_empty_string_returns_zero()
{
    int64_t out[10] = {};
    int n = parseAllowedIds("", out, 10);
    TEST_ASSERT_EQUAL(0, n);
}

void test_allow_list_null_returns_zero()
{
    int64_t out[10] = {};
    int n = parseAllowedIds(nullptr, out, 10);
    TEST_ASSERT_EQUAL(0, n);
}

void test_allow_list_negative_id_accepted()
{
    // Group chat IDs are negative integers.
    int64_t out[10] = {};
    int n = parseAllowedIds("-1001234567890", out, 10);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(-1001234567890LL, out[0]);
}

// ------------------------------------------------------------------ //
// AuthFn scan tests (inline lambda logic, mirroring main.cpp)
// ------------------------------------------------------------------ //

// Replicate the AuthFn logic so we can test it without main.cpp.
static bool authFn(int64_t fromId, const int64_t *ids, int count)
{
    if (fromId == 0) return false;
    for (int i = 0; i < count; i++)
    {
        if (fromId == ids[i]) return true;
    }
    return false;
}

void test_auth_single_entry_match()
{
    int64_t ids[] = {12345};
    TEST_ASSERT_TRUE(authFn(12345, ids, 1));
}

void test_auth_single_entry_no_match()
{
    int64_t ids[] = {12345};
    TEST_ASSERT_FALSE(authFn(99999, ids, 1));
}

void test_auth_multiple_entries_matches_first()
{
    int64_t ids[] = {111, 222, 333};
    TEST_ASSERT_TRUE(authFn(111, ids, 3));
}

void test_auth_multiple_entries_matches_last()
{
    int64_t ids[] = {111, 222, 333};
    TEST_ASSERT_TRUE(authFn(333, ids, 3));
}

void test_auth_multiple_entries_no_match()
{
    int64_t ids[] = {111, 222, 333};
    TEST_ASSERT_FALSE(authFn(444, ids, 3));
}

void test_auth_fromId_zero_always_rejected()
{
    int64_t ids[] = {0, 111, 222};
    TEST_ASSERT_FALSE(authFn(0, ids, 3));
}

void test_auth_empty_list_rejects_all()
{
    TEST_ASSERT_FALSE(authFn(12345, nullptr, 0));
}

void run_allow_list_tests()
{
    RUN_TEST(test_allow_list_single_entry);
    RUN_TEST(test_allow_list_multiple_entries);
    RUN_TEST(test_allow_list_leading_trailing_whitespace);
    RUN_TEST(test_allow_list_truncates_at_max);
    RUN_TEST(test_allow_list_skips_zero_tokens);
    RUN_TEST(test_allow_list_empty_string_returns_zero);
    RUN_TEST(test_allow_list_null_returns_zero);
    RUN_TEST(test_allow_list_negative_id_accepted);
    RUN_TEST(test_auth_single_entry_match);
    RUN_TEST(test_auth_single_entry_no_match);
    RUN_TEST(test_auth_multiple_entries_matches_first);
    RUN_TEST(test_auth_multiple_entries_matches_last);
    RUN_TEST(test_auth_multiple_entries_no_match);
    RUN_TEST(test_auth_fromId_zero_always_rejected);
    RUN_TEST(test_auth_empty_list_rejects_all);
}
