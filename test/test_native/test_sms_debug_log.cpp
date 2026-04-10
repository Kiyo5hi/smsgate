// Unit tests for SmsDebugLog persistence (RFC-0020).
//
// Coverage:
//   1. push_with_persist_calls_saveBlob
//      Wire FakePersist via setSink, push one entry, assert "smslog" blob
//      exists in FakePersist.
//
//   2. loadFrom_deserializes_previous_blob
//      Hand-craft a valid SmsLogBlob in FakePersist, call loadFrom() on a
//      fresh SmsDebugLog, assert entries match the crafted blob.
//
//   3. loadFrom_discards_wrong_version
//      Same but blob.version = 99; assert ring is empty after loadFrom.
//
//   4. ring_wraparound_round_trips
//      Push 15 entries (>10 persistent slots), persist via push(), then
//      loadFrom() into a fresh log; assert the 10 most-recent entries are
//      present in order and the 5 oldest are absent.
//
//   5. loadFrom_empty_persist_is_noop
//      Call loadFrom on a FakePersist with no "smslog" key; assert no
//      crash and ring is empty.

#include <unity.h>
#include <Arduino.h>
#include <cstring>

#include "sms_debug_log.h"
#include "fake_persist.h"

// Helper: build a minimal Entry with a given sender + outcome.
static SmsDebugLog::Entry makeEntry(const char *sender, const char *outcome,
                                    uint32_t unixTs = 0)
{
    SmsDebugLog::Entry e;
    e.sender        = String(sender);
    e.outcome       = String(outcome);
    e.unixTimestamp = unixTs;
    e.timestampMs   = 1000;
    e.bodyChars     = 5;
    return e;
}

// ---------------------------------------------------------------------------
// 1. push_with_persist_calls_saveBlob
// ---------------------------------------------------------------------------
void test_push_with_persist_calls_saveBlob()
{
    FakePersist fp;
    SmsDebugLog log;
    log.setSink(fp);

    log.push(makeEntry("+1234567890", "fwd", 1700000000u));

    // After one push, the "smslog" key should exist in FakePersist.
    SmsDebugLog::SmsLogBlob blob{};
    size_t got = fp.loadBlob("smslog", &blob, sizeof(blob));
    TEST_ASSERT_EQUAL(sizeof(SmsDebugLog::SmsLogBlob), got);
    TEST_ASSERT_EQUAL(1, blob.version);
    TEST_ASSERT_EQUAL(1, blob.count);
    TEST_ASSERT_EQUAL_STRING("+1234567890", blob.entries[0].sender);
    TEST_ASSERT_EQUAL(1700000000u, blob.entries[0].unixTimestamp);
    TEST_ASSERT_TRUE(blob.entries[0].forwarded);
}

// ---------------------------------------------------------------------------
// 2. loadFrom_deserializes_previous_blob
// ---------------------------------------------------------------------------
void test_loadFrom_deserializes_previous_blob()
{
    // Build a blob with 2 entries by hand.
    SmsDebugLog::SmsLogBlob blob{};
    blob.version = 1;
    blob.head    = 0;
    blob.count   = 2;

    std::strncpy(blob.entries[0].sender, "+111", sizeof(blob.entries[0].sender) - 1);
    std::strncpy(blob.entries[0].body,   "fwd", sizeof(blob.entries[0].body) - 1);
    blob.entries[0].unixTimestamp = 1000u;
    blob.entries[0].forwarded     = true;

    std::strncpy(blob.entries[1].sender, "+222", sizeof(blob.entries[1].sender) - 1);
    std::strncpy(blob.entries[1].body,   "err: timeout", sizeof(blob.entries[1].body) - 1);
    blob.entries[1].unixTimestamp = 2000u;
    blob.entries[1].forwarded     = false;

    FakePersist fp;
    fp.saveBlob("smslog", &blob, sizeof(blob));

    SmsDebugLog log;
    log.loadFrom(fp);

    TEST_ASSERT_EQUAL(2u, log.count());

    // dump() to inspect content (sanity check)
    String d = log.dump();
    TEST_ASSERT_TRUE(d.indexOf("+111") >= 0);
    TEST_ASSERT_TRUE(d.indexOf("+222") >= 0);
    TEST_ASSERT_TRUE(d.indexOf("fwd") >= 0);
    TEST_ASSERT_TRUE(d.indexOf("err: timeout") >= 0);
}

// ---------------------------------------------------------------------------
// 3. loadFrom_discards_wrong_version
// ---------------------------------------------------------------------------
void test_loadFrom_discards_wrong_version()
{
    SmsDebugLog::SmsLogBlob blob{};
    blob.version = 99;  // unknown version
    blob.count   = 3;
    std::strncpy(blob.entries[0].sender, "+999", sizeof(blob.entries[0].sender) - 1);

    FakePersist fp;
    fp.saveBlob("smslog", &blob, sizeof(blob));

    SmsDebugLog log;
    log.loadFrom(fp);

    // Version mismatch → ring must be empty
    TEST_ASSERT_EQUAL(0u, log.count());
}

// ---------------------------------------------------------------------------
// 4. ring_wraparound_round_trips
// ---------------------------------------------------------------------------
void test_ring_wraparound_round_trips()
{
    FakePersist fp;
    SmsDebugLog log;
    log.setSink(fp);

    // Push 15 entries; only the 10 most-recent should survive in the blob.
    for (int i = 1; i <= 15; i++)
    {
        char sender[16];
        char outcome[16];
        snprintf(sender,  sizeof(sender),  "+%d", i);
        snprintf(outcome, sizeof(outcome), "fwd-%d", i);
        log.push(makeEntry(sender, outcome, (uint32_t)(1700000000u + (unsigned)i)));
    }
    // All 15 are in RAM ring (kMaxEntries = 20).
    TEST_ASSERT_EQUAL(15u, log.count());

    // Reload into a fresh log.
    SmsDebugLog log2;
    log2.loadFrom(fp);

    // Only 10 entries should be restored.
    TEST_ASSERT_EQUAL(10u, log2.count());

    // The 10 restored entries should be entries 6..15 (the most recent).
    String d = log2.dump();
    // Entries 1-5 must be absent. The sender appears as "| +N\n" in the dump
    // so we can match "| +N\n" to avoid "+1" matching "+10", "+11", etc.
    for (int i = 1; i <= 5; i++)
    {
        char pattern[20];
        snprintf(pattern, sizeof(pattern), "| +%d\n", i);
        TEST_ASSERT_EQUAL(-1, d.indexOf(String(pattern)));
    }
    // Entries 6-15 must be present.
    for (int i = 6; i <= 15; i++)
    {
        char pattern[20];
        snprintf(pattern, sizeof(pattern), "| +%d\n", i);
        TEST_ASSERT_TRUE(d.indexOf(String(pattern)) >= 0);
    }
}

// ---------------------------------------------------------------------------
// 5. loadFrom_empty_persist_is_noop
// ---------------------------------------------------------------------------
void test_loadFrom_empty_persist_is_noop()
{
    FakePersist fp;  // "smslog" key absent

    SmsDebugLog log;
    log.loadFrom(fp);

    TEST_ASSERT_EQUAL(0u, log.count());
    // dump() must not crash and return the empty message
    String d = log.dump();
    TEST_ASSERT_TRUE(d.indexOf("no SMS logged") >= 0);
}

// ---------------------------------------------------------------------------
// RFC-0117: dumpBriefFiltered tests
// ---------------------------------------------------------------------------
void test_dumpBriefFiltered_returns_matching_entries_only()
{
    SmsDebugLog log;
    log.push(makeEntry("+8613800138000", "in:forwarded"));
    log.push(makeEntry("+447911123456",  "out:sent"));
    log.push(makeEntry("+8613988776655", "in:forwarded"));

    String result = log.dumpBriefFiltered(10, String("+8613"));
    // Should include both +8613 entries.
    TEST_ASSERT_TRUE(result.indexOf(String("+86138")) >= 0);
    TEST_ASSERT_TRUE(result.indexOf(String("+86139")) >= 0);
    // Should NOT include the UK number.
    TEST_ASSERT_EQUAL(-1, result.indexOf(String("+4479")));
}

void test_dumpBriefFiltered_no_match_returns_placeholder()
{
    SmsDebugLog log;
    log.push(makeEntry("+8613800138000", "in:forwarded"));

    String result = log.dumpBriefFiltered(10, String("+1555"));
    TEST_ASSERT_TRUE(result.indexOf(String("+1555")) >= 0); // filter in placeholder
    TEST_ASSERT_TRUE(result.indexOf(String("no entries")) >= 0);
}

void test_dumpBriefFiltered_empty_log_returns_placeholder()
{
    SmsDebugLog log;
    String result = log.dumpBriefFiltered(10, String("+1"));
    TEST_ASSERT_TRUE(result.indexOf(String("no entries")) >= 0);
}

void test_dumpBriefFiltered_respects_n_limit()
{
    SmsDebugLog log;
    for (int i = 0; i < 5; i++)
        log.push(makeEntry("+8613800138000", "in:forwarded"));

    // Ask for only 2 matches out of 5.
    String result = log.dumpBriefFiltered(2, String("+8613"));
    // Count newlines: expect exactly 2 result lines.
    int lines = 0;
    for (unsigned int i = 0; i < result.length(); i++)
        if (result[i] == '\n') lines++;
    TEST_ASSERT_EQUAL(2, lines);
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------
void run_sms_debug_log_tests()
{
    RUN_TEST(test_push_with_persist_calls_saveBlob);
    RUN_TEST(test_loadFrom_deserializes_previous_blob);
    RUN_TEST(test_loadFrom_discards_wrong_version);
    RUN_TEST(test_ring_wraparound_round_trips);
    RUN_TEST(test_loadFrom_empty_persist_is_noop);
    // RFC-0117: dumpBriefFiltered
    RUN_TEST(test_dumpBriefFiltered_returns_matching_entries_only);
    RUN_TEST(test_dumpBriefFiltered_no_match_returns_placeholder);
    RUN_TEST(test_dumpBriefFiltered_empty_log_returns_placeholder);
    RUN_TEST(test_dumpBriefFiltered_respects_n_limit);
}
