#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "ipersist.h"

// Ring buffer mapping `Telegram message_id -> SMS sender phone number`
// (RFC-0003 §2). Each forwarded SMS writes one slot keyed by
// `message_id % kSlotCount`, storing both the message_id and the
// sender phone. When a future Telegram update arrives carrying a
// `reply_to_message.message_id`, we look up that slot and ONLY trust
// the stored phone number if the slot's stored message_id matches
// exactly — otherwise the slot has been overwritten by a newer
// message and the reply is too old to route.
//
// Why a ring buffer rather than an LRU map: Telegram message_ids are
// monotonically increasing per chat, so `id % N` gives an implicit
// "last N" window with no LRU bookkeeping. The cap of 200 is the
// RFC's recommendation and gives us ~1 day of headroom for an SMS
// every 7 minutes, which dwarfs realistic traffic.
//
// Persistence: the slot table is serialized to a fixed-size byte
// blob and stored via IPersist. The serialization format is a
// versioned header followed by `kSlotCount` packed slot records.
// Format mismatches (different version, different slot count, etc.)
// fall back to an empty table on load.
//
// Phone numbers are clamped to kPhoneMax bytes including the NUL
// terminator. International numbers ("+861234..." style) are well
// under that. We use a fixed-size char array, not a String, so the
// blob serialization is trivial and the layout is stable across
// firmware versions.
class ReplyTargetMap
{
public:
    static constexpr size_t kSlotCount = 200;
    static constexpr size_t kPhoneMax = 23; // 22 chars + NUL — E.164 max is 15 + slack

    // Layout version, bumped if the serialized format ever changes.
    static constexpr uint16_t kFormatVersion = 1;

    explicit ReplyTargetMap(IPersist &persist);

    // Load the slot table from persist. Safe to call repeatedly;
    // each call wipes any in-RAM state and reloads. If the stored
    // blob is missing, malformed, or a different version, the
    // in-RAM table is reset to all-empty.
    void load();

    // Persist the current slot table back to persist. Called from
    // put() after every successful insert.
    void save();

    // Insert a (message_id, phone) pair. Overwrites whatever was in
    // the slot indexed by `messageId % kSlotCount`. Truncates phone
    // numbers longer than kPhoneMax-1 chars (logged at warn level).
    // After insertion, immediately persists.
    void put(int32_t messageId, const String &phone);

    // Look up by message_id. Returns true and fills `outPhone` iff
    // a slot exists for this message_id AND the stored message_id
    // matches exactly (otherwise the slot has been overwritten).
    bool lookup(int32_t messageId, String &outPhone) const;

    // ---- Test-only introspection ----

    // Returns the raw stored message_id at slot `messageId %
    // kSlotCount` regardless of whether it matches. 0 = empty.
    int32_t slotMessageIdFor(int32_t messageId) const;

    // Number of non-empty slots. O(N).
    size_t occupiedSlots() const;

private:
    struct Slot
    {
        int32_t messageId = 0; // 0 = empty
        char phone[kPhoneMax] = {0};
    };

    // Serialized blob layout:
    //   uint16 version
    //   uint16 slot_count
    //   uint16 phone_max
    //   uint16 reserved   (alignment)
    //   Slot[slot_count]
    struct Header
    {
        uint16_t version;
        uint16_t slotCount;
        uint16_t phoneMax;
        uint16_t reserved;
    };

    static constexpr size_t kBlobSize = sizeof(Header) + sizeof(Slot) * kSlotCount;

    IPersist &persist_;
    Slot slots_[kSlotCount];
};
