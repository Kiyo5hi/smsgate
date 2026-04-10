#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "ipersist.h"

// In-RAM ring buffer of the last N SMS diagnostic records.
// Captures the metadata that's impossible to recover after the fact:
// was concat UDH present? what DCS / encoding? how many chars? what
// happened (forwarded, buffered, error)?
//
// Accessible via a Telegram "/debug" command so the user can diagnose
// truncation / reassembly issues without a serial connection.
//
// RFC-0020: Persistence layer. Call loadFrom(p) at boot to restore the
// last 10 entries from NVS, and setSink(p) so each push() automatically
// persists the 10 most-recent entries to NVS via saveBlob("smslog", ...).
class SmsDebugLog
{
public:
    struct Entry
    {
        unsigned long timestampMs = 0; // millis() at receive
        String sender;
        uint16_t bodyChars = 0;      // decoded body length (chars)
        bool isConcat = false;
        uint16_t concatRef = 0;
        uint8_t concatTotal = 0;
        uint8_t concatPart = 0;
        String outcome;              // "fwd", "buf", "err: ..."
        String pduPrefix;            // first 120 hex chars of raw PDU
        uint32_t unixTimestamp = 0;  // seconds since epoch (0 = unknown)
    };

    static constexpr size_t kMaxEntries = 20;

    void push(const Entry &e);

    // RFC-0020: Load up to 10 persisted entries from NVS into the RAM ring.
    // Call once in setup(), before setSink(), so the boot-loaded entries
    // are visible before any new SMS arrive.
    void loadFrom(IPersist &p);

    // RFC-0020: Register a persistence sink. After every push() the 10
    // most-recent entries are serialized and written to NVS via
    // p.saveBlob("smslog", ...). Set once in setup().
    void setSink(IPersist &p) { persist_ = &p; }

    // Format all entries oldest-first as a Telegram-friendly string.
    // Fits in a single 4096-char message for 20 entries.
    String dump() const;

    size_t count() const { return count_; }

    // RFC-0040: Clear all entries from the ring and (if a sink is set)
    // persist the empty log to NVS so it survives a reboot.
    void clear()
    {
        head_   = 0;
        count_  = 0;
        if (persist_)
            persist(); // writes empty blob to NVS
    }

    // -----------------------------------------------------------------------
    // RFC-0020: NVS blob layout
    // Fields are ordered for natural alignment — no compiler padding expected.
    // sizeof(PersistEntry) == 170, sizeof(SmsLogBlob) == 1704.
    // The static_asserts below will catch any unexpected padding at
    // compile time.
    // -----------------------------------------------------------------------

    struct PersistEntry
    {
        uint32_t unixTimestamp;  //  4 bytes — seconds since epoch (0 = unknown)
        bool     forwarded;      //  1 byte  — true if outcome starts with "fwd"
        char     sender[21];     // 21 bytes — E.164 number + NUL (offset 5)
        char     body[101];      // 101 bytes — first 100 outcome chars + NUL
        char     error[41];      //  41 bytes — pduPrefix (40 chars) + NUL
        //                          total: 4+1+21+101+41 = 168 (multiple of 4,
        //                          no tail padding on any standard ABI)
    };

    struct SmsLogBlob
    {
        uint8_t     version;        // 1 byte — bump on schema change; current = 1
        uint8_t     head;           // 1 byte — index of oldest entry in entries[]
        uint8_t     count;          // 1 byte — number of valid entries (0..10)
        uint8_t     _pad;           // 1 byte — alignment filler
        PersistEntry entries[10];   // 10 × 168 = 1680 bytes
        //                             total header: 4, total: 1684
    };

private:
    void persist();  // Serialize to NVS; called after push() when persist_ != nullptr

    Entry entries_[kMaxEntries];
    size_t head_ = 0;   // next write position
    size_t count_ = 0;

    IPersist *persist_ = nullptr;  // RFC-0020: NVS sink; nullptr = no persistence
};

// sizeof(PersistEntry) = 4+1+21+101+41 = 168 (multiple of 4, no tail padding).
// sizeof(SmsLogBlob)   = 4 + 10*168    = 1684.
static_assert(sizeof(SmsDebugLog::PersistEntry) == 168, "PersistEntry size changed");
static_assert(sizeof(SmsDebugLog::SmsLogBlob)   == 1684, "SmsLogBlob size changed");
