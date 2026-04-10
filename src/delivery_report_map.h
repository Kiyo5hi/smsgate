#pragma once

#include <Arduino.h>
#include <stdint.h>

// Ring buffer mapping TP-MR (0-255) to pending delivery report info
// (RFC-0011). After calling IModem::sendPduSms the caller stores the
// returned MR here so a future +CDS URC can be correlated back to the
// destination phone number.
//
// Design:
//   - 32 slots, indexed by `mr % kSlotCount`. 32 >> realistic in-flight
//     window for a personal bridge.
//   - Each slot stores the MR to guard against wrap-around collisions
//     (same pattern as ReplyTargetMap).
//   - 1-hour TTL. If the SMSC never reports, the slot expires silently.
//   - No NVS persistence. Delivery reports are ephemeral; a reboot loses
//     in-flight correlations and the user simply never sees "Delivered".
//     This is acceptable — the "sent" confirmation is already posted.
//   - Not thread-safe; accessed only from the main Arduino loop().
class DeliveryReportMap
{
public:
    static constexpr size_t kSlotCount = 32;
    static constexpr size_t kPhoneMax  = 23; // 22 chars + NUL
    static constexpr uint32_t kTtlMs   = 3600000UL; // 1 hour

    // Store a pending delivery report entry.
    // `mr`     : TP-MR as returned by IModem::sendPduSms (0-255).
    // `phone`  : destination phone number (will be truncated if > kPhoneMax-1 chars).
    // `nowMs`  : current millis() for TTL tracking.
    void put(uint8_t mr, const String &phone, uint32_t nowMs)
    {
        size_t idx = mr % kSlotCount;
        slots_[idx].mr        = mr;
        slots_[idx].timestamp = nowMs;
        // Copy phone with NUL termination, truncating if necessary.
        size_t len = phone.length();
        if (len >= kPhoneMax)
            len = kPhoneMax - 1;
        memcpy(slots_[idx].phone, phone.c_str(), len);
        slots_[idx].phone[len] = '\0';
        slots_[idx].occupied = true;
    }

    // Look up by MR. Returns true and fills `outPhone` iff:
    //   - the slot for `mr % kSlotCount` is occupied,
    //   - the stored MR matches exactly (no wrap-around collision), AND
    //   - the entry is not older than kTtlMs.
    // On success the slot is consumed (cleared) so a second lookup
    // for the same MR returns false — one report per outbound SMS.
    bool lookup(uint8_t mr, String &outPhone, uint32_t nowMs)
    {
        size_t idx = mr % kSlotCount;
        if (!slots_[idx].occupied)
            return false;
        if (slots_[idx].mr != mr)
            return false;
        uint32_t age = nowMs - slots_[idx].timestamp;
        if (age >= kTtlMs)
        {
            // Expired — evict and report not found.
            slots_[idx].occupied = false;
            return false;
        }
        outPhone = String(slots_[idx].phone);
        slots_[idx].occupied = false; // consume
        return true;
    }

    // Evict all entries older than kTtlMs. Call periodically from loop().
    void evictExpired(uint32_t nowMs)
    {
        for (size_t i = 0; i < kSlotCount; ++i)
        {
            if (!slots_[i].occupied)
                continue;
            uint32_t age = nowMs - slots_[i].timestamp;
            if (age >= kTtlMs)
                slots_[i].occupied = false;
        }
    }

    // Number of occupied (non-expired) slots. For testing / introspection.
    size_t occupiedSlots() const
    {
        size_t count = 0;
        for (size_t i = 0; i < kSlotCount; ++i)
            if (slots_[i].occupied)
                ++count;
        return count;
    }

private:
    struct Slot
    {
        uint8_t  mr        = 0;
        char     phone[kPhoneMax] = {0};
        uint32_t timestamp = 0;
        bool     occupied  = false;
    };

    Slot slots_[kSlotCount];
};
