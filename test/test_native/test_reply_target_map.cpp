// Unit tests for src/reply_target_map.{h,cpp}.
//
// Coverage:
//   - put / lookup happy path
//   - lookup miss on empty slot
//   - ring-buffer collision: a newer message id overwrites a slot
//     and lookup of the OLD id at that slot returns false
//   - persistence round-trip via FakePersist
//   - format-mismatch on load is recovered as empty
//   - phone truncation for too-long inputs

#include <unity.h>
#include <Arduino.h>

#include "reply_target_map.h"
#include "fake_persist.h"

void test_ReplyTargetMap_put_lookup_roundtrip()
{
    FakePersist persist;
    ReplyTargetMap map(persist);
    map.load();

    map.put(42, String("+8613800138000"));
    String phone;
    TEST_ASSERT_TRUE(map.lookup(42, phone));
    TEST_ASSERT_EQUAL_STRING("+8613800138000", phone.c_str());
}

void test_ReplyTargetMap_lookup_empty_slot_returns_false()
{
    FakePersist persist;
    ReplyTargetMap map(persist);
    map.load();

    String phone = String("untouched");
    TEST_ASSERT_FALSE(map.lookup(99, phone));
}

void test_ReplyTargetMap_lookup_zero_returns_false()
{
    FakePersist persist;
    ReplyTargetMap map(persist);
    map.load();
    map.put(42, String("+861234"));
    String phone;
    TEST_ASSERT_FALSE(map.lookup(0, phone));
    TEST_ASSERT_FALSE(map.lookup(-1, phone));
}

void test_ReplyTargetMap_collision_overwrites_slot()
{
    // 42 and 42 + 200 hash to the same slot. Writing the second one
    // overwrites the first; lookup of 42 must then return false.
    FakePersist persist;
    ReplyTargetMap map(persist);
    map.load();

    map.put(42, String("+86A"));
    String phone;
    TEST_ASSERT_TRUE(map.lookup(42, phone));
    TEST_ASSERT_EQUAL_STRING("+86A", phone.c_str());

    int32_t collidingId = 42 + (int32_t)ReplyTargetMap::kSlotCount;
    map.put(collidingId, String("+86B"));

    // Old id should now miss.
    phone = String("untouched");
    TEST_ASSERT_FALSE(map.lookup(42, phone));
    // New id should hit.
    TEST_ASSERT_TRUE(map.lookup(collidingId, phone));
    TEST_ASSERT_EQUAL_STRING("+86B", phone.c_str());
}

void test_ReplyTargetMap_persistence_across_restart()
{
    FakePersist persist;
    {
        ReplyTargetMap map1(persist);
        map1.load();
        map1.put(7, String("+861111111"));
        map1.put(15, String("+862222222"));
    }
    // "Restart" — new map instance, same persist.
    {
        ReplyTargetMap map2(persist);
        map2.load();
        String phone;
        TEST_ASSERT_TRUE(map2.lookup(7, phone));
        TEST_ASSERT_EQUAL_STRING("+861111111", phone.c_str());
        TEST_ASSERT_TRUE(map2.lookup(15, phone));
        TEST_ASSERT_EQUAL_STRING("+862222222", phone.c_str());
    }
}

void test_ReplyTargetMap_load_empty_persist_is_empty()
{
    FakePersist persist;
    ReplyTargetMap map(persist);
    map.load();
    TEST_ASSERT_EQUAL(0, (int)map.occupiedSlots());
    String phone;
    TEST_ASSERT_FALSE(map.lookup(1, phone));
}

void test_ReplyTargetMap_load_garbage_recovers_empty()
{
    // Stuff a totally bogus blob into persist; load should drop it
    // and start with an empty table rather than crashing.
    FakePersist persist;
    uint8_t garbage[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    persist.saveReplyTargets(garbage, sizeof(garbage));

    ReplyTargetMap map(persist);
    map.load();
    TEST_ASSERT_EQUAL(0, (int)map.occupiedSlots());
}

void test_ReplyTargetMap_phone_truncation_does_not_corrupt_neighbors()
{
    // Way-too-long phone number should be silently truncated; the
    // next slot must remain unaffected.
    FakePersist persist;
    ReplyTargetMap map(persist);
    map.load();

    String huge;
    for (int i = 0; i < 100; ++i)
        huge += '9';
    map.put(50, huge);
    map.put(51, String("+8612"));

    String phone;
    TEST_ASSERT_TRUE(map.lookup(50, phone));
    // Truncated to kPhoneMax-1 = 22 chars.
    TEST_ASSERT_EQUAL(22, (int)phone.length());

    TEST_ASSERT_TRUE(map.lookup(51, phone));
    TEST_ASSERT_EQUAL_STRING("+8612", phone.c_str());
}

void test_ReplyTargetMap_save_called_per_put()
{
    FakePersist persist;
    ReplyTargetMap map(persist);
    map.load();
    int before = persist.saveReplyTargetsCalls();
    map.put(1, String("+861"));
    map.put(2, String("+862"));
    map.put(3, String("+863"));
    TEST_ASSERT_EQUAL(before + 3, persist.saveReplyTargetsCalls());
}

void run_reply_target_map_tests()
{
    RUN_TEST(test_ReplyTargetMap_put_lookup_roundtrip);
    RUN_TEST(test_ReplyTargetMap_lookup_empty_slot_returns_false);
    RUN_TEST(test_ReplyTargetMap_lookup_zero_returns_false);
    RUN_TEST(test_ReplyTargetMap_collision_overwrites_slot);
    RUN_TEST(test_ReplyTargetMap_persistence_across_restart);
    RUN_TEST(test_ReplyTargetMap_load_empty_persist_is_empty);
    RUN_TEST(test_ReplyTargetMap_load_garbage_recovers_empty);
    RUN_TEST(test_ReplyTargetMap_phone_truncation_does_not_corrupt_neighbors);
    RUN_TEST(test_ReplyTargetMap_save_called_per_put);
}
