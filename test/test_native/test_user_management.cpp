// Unit tests for the RFC-0019 ListMutatorFn lambda logic.
//
// These tests exercise the mutation logic in isolation — without going through
// TelegramPoller — to verify add/remove semantics, duplicate detection,
// capacity limits, and NVS blob round-trips via FakePersist.
//
// The lambda under test is the same shape as the one wired in main.cpp.
// We instantiate a local version with controlled allowedIds[] / runtimeIds[]
// to keep tests hermetic.

#include <unity.h>
#include <Arduino.h>
#include <functional>
#include <cstring>

#include "fake_persist.h"
#include "telegram_poller.h" // for ListMutatorFn type alias

// ---------------------------------------------------------------------------
// Test fixture: mirrors the file-scope statics and lambda from main.cpp.
// Each test creates its own instance so state doesn't leak between tests.
// ---------------------------------------------------------------------------

struct UserMgmtFixture
{
    // Compile-time allow list (two entries; index 0 is admin).
    int64_t allowedIds[10] = {};
    int     allowedIdCount = 0;

    // Runtime list.
    int64_t runtimeIds[10] = {};
    int     runtimeIdCount = 0;

    FakePersist persist;

    // Build a mutator lambda that closes over this fixture's state.
    TelegramPoller::ListMutatorFn makeMutator()
    {
        return [this](int64_t callerId, const String &cmd,
                      int64_t targetId, String &reason) -> bool {
            bool isAdmin = false;
            for (int i = 0; i < allowedIdCount; i++)
            {
                if (callerId == allowedIds[i]) { isAdmin = true; break; }
            }

            if (cmd == "list")
            {
                String out = "Compile-time users (" + String(allowedIdCount) + "):\n";
                for (int i = 0; i < allowedIdCount; i++)
                {
                    out += "  " + String((long long)allowedIds[i]);
                    if (i == 0) out += " [admin]";
                    out += "\n";
                }
                out += "Runtime users (" + String(runtimeIdCount) + "):\n";
                for (int i = 0; i < runtimeIdCount; i++)
                {
                    out += "  " + String((long long)runtimeIds[i]) + "\n";
                }
                out += "Total: " + String(allowedIdCount + runtimeIdCount);
                reason = out;
                return true;
            }

            if (!isAdmin)
            {
                reason = String("Permission denied. Only compile-time users may manage the user list.");
                return false;
            }

            if (cmd == "add")
            {
                for (int i = 0; i < allowedIdCount; i++)
                {
                    if (targetId == allowedIds[i])
                    {
                        reason = String("User is already a compile-time admin user.");
                        return false;
                    }
                }
                for (int i = 0; i < runtimeIdCount; i++)
                {
                    if (targetId == runtimeIds[i])
                    {
                        reason = String("User is already in the runtime list.");
                        return false;
                    }
                }
                if (runtimeIdCount >= 10)
                {
                    reason = String("Runtime user list is full (maximum 10). Remove a user first.");
                    return false;
                }
                runtimeIds[runtimeIdCount++] = targetId;
            }
            else if (cmd == "remove")
            {
                for (int i = 0; i < allowedIdCount; i++)
                {
                    if (targetId == allowedIds[i])
                    {
                        reason = String("Cannot remove a compile-time user. Edit secrets.h and reflash.");
                        return false;
                    }
                }
                int idx = -1;
                for (int i = 0; i < runtimeIdCount; i++)
                {
                    if (runtimeIds[i] == targetId) { idx = i; break; }
                }
                if (idx < 0)
                {
                    reason = String("User ") + String((long long)targetId) + " not in the runtime list.";
                    return false;
                }
                for (int i = idx; i < runtimeIdCount - 1; i++)
                {
                    runtimeIds[i] = runtimeIds[i + 1];
                }
                runtimeIdCount--;
            }

            // Persist.
            struct { int32_t count; int64_t ids[10]; } blob{};
            blob.count = runtimeIdCount;
            memcpy(blob.ids, runtimeIds, (size_t)runtimeIdCount * sizeof(int64_t));
            persist.saveBlob("ulist", &blob, sizeof(blob));
            return true;
        };
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Add to an empty list: count becomes 1, NVS written.
void test_UserMgmt_add_to_empty_list()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIdCount = 1;

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(1000 /*admin*/, String("add"), 2000, reason);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, f.runtimeIdCount);
    TEST_ASSERT_EQUAL_INT64(2000, f.runtimeIds[0]);

    // NVS was written.
    struct { int32_t count; int64_t ids[10]; } blob{};
    size_t got = f.persist.loadBlob("ulist", &blob, sizeof(blob));
    TEST_ASSERT_TRUE(got >= sizeof(int32_t));
    TEST_ASSERT_EQUAL(1, blob.count);
    TEST_ASSERT_EQUAL_INT64(2000, blob.ids[0]);
}

// Add to a full list (count == 10): returns false, reason "full".
void test_UserMgmt_add_to_full_list_denied()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIdCount = 1;
    f.runtimeIdCount = 10;
    for (int i = 0; i < 10; i++) f.runtimeIds[i] = (int64_t)(2000 + i);

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(1000 /*admin*/, String("add"), 9999, reason);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(10, f.runtimeIdCount); // unchanged
    TEST_ASSERT_TRUE(reason.indexOf(String("full")) >= 0);
}

// Add a duplicate already in the runtime list: returns false, reason "already in runtime".
void test_UserMgmt_add_duplicate_runtime_denied()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIdCount = 1;
    f.runtimeIds[0] = 5555;
    f.runtimeIdCount = 1;

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(1000, String("add"), 5555, reason);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(1, f.runtimeIdCount); // unchanged
    TEST_ASSERT_TRUE(reason.indexOf(String("already in the runtime")) >= 0);
}

// Add an ID already in allowedIds (compile-time list): returns false.
void test_UserMgmt_add_duplicate_compiletime_denied()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIds[1] = 1001;
    f.allowedIdCount = 2;

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(1000, String("add"), 1001, reason);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(0, f.runtimeIdCount);
    TEST_ASSERT_TRUE(reason.indexOf(String("compile-time")) >= 0);
}

// Remove from a list of 1: count becomes 0, NVS written.
void test_UserMgmt_remove_sole_entry()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIdCount = 1;
    f.runtimeIds[0] = 7777;
    f.runtimeIdCount = 1;

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(1000, String("remove"), 7777, reason);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(0, f.runtimeIdCount);

    // NVS was written with count=0.
    struct { int32_t count; int64_t ids[10]; } blob{};
    size_t got = f.persist.loadBlob("ulist", &blob, sizeof(blob));
    TEST_ASSERT_TRUE(got >= sizeof(int32_t));
    TEST_ASSERT_EQUAL(0, blob.count);
}

// Remove an ID not in the list: returns false, reason "not in the runtime list".
void test_UserMgmt_remove_not_found_denied()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIdCount = 1;
    f.runtimeIds[0] = 7777;
    f.runtimeIdCount = 1;

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(1000, String("remove"), 8888, reason);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(1, f.runtimeIdCount); // unchanged
    TEST_ASSERT_TRUE(reason.indexOf(String("not in the runtime")) >= 0);
}

// Remove an ID that is in the compile-time list: returns false.
void test_UserMgmt_remove_compiletime_user_denied()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIds[1] = 1001;
    f.allowedIdCount = 2;

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(1000, String("remove"), 1001, reason);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(reason.indexOf(String("compile-time")) >= 0);
}

// Non-admin caller: /adduser is denied.
void test_UserMgmt_add_from_non_admin_denied()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIdCount = 1;
    f.runtimeIds[0] = 9000; // 9000 is a runtime user, not admin
    f.runtimeIdCount = 1;

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(9000 /*runtime, not admin*/, String("add"), 9999, reason);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(1, f.runtimeIdCount); // unchanged
    TEST_ASSERT_TRUE(reason.indexOf(String("Permission denied")) >= 0);
}

// Non-admin caller: /removeuser is denied.
void test_UserMgmt_remove_from_non_admin_denied()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIdCount = 1;
    f.runtimeIds[0] = 9000;
    f.runtimeIds[1] = 9001;
    f.runtimeIdCount = 2;

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(9000, String("remove"), 9001, reason);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(2, f.runtimeIdCount); // unchanged
    TEST_ASSERT_TRUE(reason.indexOf(String("Permission denied")) >= 0);
}

// /listusers: any authorized user may call it (no admin check for "list").
void test_UserMgmt_listusers_accessible_by_runtime_user()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIdCount = 1;
    f.runtimeIds[0] = 9000;
    f.runtimeIdCount = 1;

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(9000 /*runtime user, not admin*/, String("list"), 0, reason);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(reason.indexOf(String("Compile-time users")) >= 0);
    TEST_ASSERT_TRUE(reason.indexOf(String("Runtime users")) >= 0);
    TEST_ASSERT_TRUE(reason.indexOf(String("9000")) >= 0);
}

// NVS blob round-trip: write count=2, reload from FakePersist, verify.
void test_UserMgmt_nvs_blob_roundtrip()
{
    FakePersist persist;

    // Simulate a save.
    struct { int32_t count; int64_t ids[10]; } blob{};
    blob.count = 2;
    blob.ids[0] = 111111;
    blob.ids[1] = 222222;
    persist.saveBlob("ulist", &blob, sizeof(blob));

    // Simulate a load.
    struct { int32_t count; int64_t ids[10]; } loaded{};
    size_t got = persist.loadBlob("ulist", &loaded, sizeof(loaded));

    TEST_ASSERT_EQUAL(sizeof(blob), got);
    TEST_ASSERT_EQUAL(2, loaded.count);
    TEST_ASSERT_EQUAL_INT64(111111, loaded.ids[0]);
    TEST_ASSERT_EQUAL_INT64(222222, loaded.ids[1]);
}

// loadBlob on an absent key returns 0.
void test_UserMgmt_nvs_absent_key_returns_zero()
{
    FakePersist persist;
    struct { int32_t count; int64_t ids[10]; } blob{};
    size_t got = persist.loadBlob("ulist", &blob, sizeof(blob));
    TEST_ASSERT_EQUAL(0, (int)got);
}

// Remove middle entry from a list of 3: entries shift left correctly.
void test_UserMgmt_remove_middle_entry_shifts_correctly()
{
    UserMgmtFixture f;
    f.allowedIds[0] = 1000;
    f.allowedIdCount = 1;
    f.runtimeIds[0] = 100;
    f.runtimeIds[1] = 200;
    f.runtimeIds[2] = 300;
    f.runtimeIdCount = 3;

    auto mutator = f.makeMutator();
    String reason;
    bool ok = mutator(1000, String("remove"), 200, reason);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(2, f.runtimeIdCount);
    TEST_ASSERT_EQUAL_INT64(100, f.runtimeIds[0]);
    TEST_ASSERT_EQUAL_INT64(300, f.runtimeIds[1]);
}

void run_user_management_tests()
{
    RUN_TEST(test_UserMgmt_add_to_empty_list);
    RUN_TEST(test_UserMgmt_add_to_full_list_denied);
    RUN_TEST(test_UserMgmt_add_duplicate_runtime_denied);
    RUN_TEST(test_UserMgmt_add_duplicate_compiletime_denied);
    RUN_TEST(test_UserMgmt_remove_sole_entry);
    RUN_TEST(test_UserMgmt_remove_not_found_denied);
    RUN_TEST(test_UserMgmt_remove_compiletime_user_denied);
    RUN_TEST(test_UserMgmt_add_from_non_admin_denied);
    RUN_TEST(test_UserMgmt_remove_from_non_admin_denied);
    RUN_TEST(test_UserMgmt_listusers_accessible_by_runtime_user);
    RUN_TEST(test_UserMgmt_nvs_blob_roundtrip);
    RUN_TEST(test_UserMgmt_nvs_absent_key_returns_zero);
    RUN_TEST(test_UserMgmt_remove_middle_entry_shifts_correctly);
}
