#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <functional>
#include <vector>

#include "imodem.h"
#include "ibot_client.h"
#include "sms_codec.h"

// Forward declaration — sms_handler doesn't otherwise need to know
// about reply target storage; it just calls put() on the pointer.
class ReplyTargetMap;

// Reboot callback injected from the composition root. Production code
// passes a lambda that calls `ESP.restart()`; tests pass a lambda that
// bumps a counter so the reboot path is actually exercised.
using RebootFn = std::function<void()>;

// Clock callback: returns a monotonic millis() value. Production wires
// this to `millis()`; tests pass a mutable lambda so TTL-dependent
// paths are deterministic. Granularity is milliseconds so we can TTL
// against real wall-clock hours in production and against arbitrary
// epochs in tests.
using ClockFn = std::function<unsigned long()>;

// Stateful SMS pipeline. Owns the consecutive-failure counter and the
// reboot threshold; depends only on the injected IModem, IBotClient,
// and RebootFn. No globals, no Arduino hardware access, so the whole
// class is reachable from the native test env.
//
// With RFC-0002 the handler also owns an in-memory reassembly buffer
// for concatenated SMS:
//   - Keyed by (sender, ref_number).
//   - 24-hour TTL from the first fragment per key.
//   - Max 8 concurrent keys (LRU eviction on overflow).
//   - Max 2 KB of body bytes per key (oversized fragments rejected).
//   - Max 8 KB body bytes total across all keys (LRU eviction).
// On eviction or TTL expiry the partial fragments are dropped from
// memory; a reboot rehydrates them from the SIM via sweepExistingSms.
class SmsHandler
{
public:
    // After this many consecutive Telegram send failures, the handler
    // calls the injected RebootFn to escape stuck TLS / WiFi / DNS /
    // TinyGSM states. Public so tests can reference it by name.
    static constexpr int MAX_CONSECUTIVE_FAILURES = 8;

    // Reassembly buffer caps (RFC-0002). Public so tests can reference
    // them by name instead of hard-coding magic numbers.
    static constexpr size_t MAX_CONCAT_KEYS = 8;
    static constexpr size_t MAX_BYTES_PER_KEY = 2 * 1024;
    static constexpr size_t MAX_BYTES_TOTAL = 8 * 1024;
    static constexpr unsigned long CONCAT_TTL_MS = 24UL * 60UL * 60UL * 1000UL;

    // Clock defaults to `millis()` if not supplied.
    SmsHandler(IModem &modem, IBotClient &bot, RebootFn reboot, ClockFn clock = nullptr);

    // Optional: attach a ReplyTargetMap (RFC-0003 §2). When set, every
    // successful SMS forward writes a (telegram_message_id, sms_sender)
    // entry into the map so a future Telegram reply can route back
    // to the original SMS sender. Pass nullptr (or just don't call
    // this) to disable the bidirectional path.
    void setReplyTargetMap(ReplyTargetMap *map) { replyTargets_ = map; }

    // Read the SMS at SIM index <idx>, forward it to the bot, and
    // delete it from the SIM on success. Leaves the SMS in place on
    // failure so a later retry can pick it up. After
    // MAX_CONSECUTIVE_FAILURES the reboot callback fires.
    //
    // For concatenated SMS: fragments are buffered in memory until the
    // full message can be assembled. Incomplete fragments are kept on
    // the SIM (NOT deleted) so a reboot rehydrates them. Once all
    // parts arrive, the assembled message is forwarded and ALL
    // contributing SIM slots are deleted on success.
    void handleSmsIndex(int idx);

    // Drain every SMS currently on the SIM via AT+CMGL. Used at startup
    // to catch up on messages that arrived while the bridge was offline.
    void sweepExistingSms();

    // Test-only accessor — no runtime caller.
    int consecutiveFailures() const { return consecutiveFailures_; }

    // Test-only accessor — returns number of in-flight concat groups.
    size_t concatKeyCount() const { return concatGroups_.size(); }

private:
    // Per-fragment record. We keep the raw decoded content so we can
    // reassemble when the last part arrives.
    struct ConcatFragment
    {
        uint8_t partNumber = 0; // 1-indexed per 3GPP
        int simIndex = -1;      // SIM storage slot for +CMGD cleanup
        String content;         // decoded (UTF-8) body for this part
    };

    // Reassembly bucket, one per (sender, ref_number).
    struct ConcatGroup
    {
        String sender;          // for formatted header on completion
        String firstTimestamp;  // timestamp of the FIRST fragment we saw
        uint16_t refNumber = 0;
        uint8_t totalParts = 0;
        std::vector<ConcatFragment> fragments;
        unsigned long firstSeenMs = 0;
        unsigned long lastSeenMs = 0;
        size_t byteCount = 0;   // sum of fragment content byte lengths
    };

    // Forward the full message (or a single-part PDU) to Telegram.
    // Returns true iff the bot accepted it.
    bool forwardSingle(const sms_codec::SmsPdu &pdu, int simIndex);

    // Insert a fragment into the reassembly buffer, possibly
    // completing a group. Returns true if the group is now complete
    // and was posted successfully (so the caller knows to delete all
    // contributing SIM slots). If the group is still incomplete or
    // eviction happened, returns false and the caller leaves its
    // own SIM slot in place.
    //
    // On Telegram failure during completion, `deleteSlots` is NOT
    // populated and the fragments are put back in the group so a
    // later retry can try again (after the global failure counter
    // hits the threshold, the ESP reboots and we start fresh from
    // the SIM).
    bool insertFragmentAndMaybePost(const sms_codec::SmsPdu &pdu, int simIndex,
                                    std::vector<int> &deleteSlots);

    // Drop expired / overflowed groups. LRU by lastSeenMs.
    void evictExpiredLocked(unsigned long now);
    void evictLruUntilUnderCaps(size_t reservedExtraBytes);

    // Look up a group by (sender, ref). Returns nullptr if missing.
    ConcatGroup *findGroup(const String &sender, uint16_t ref);

    // Bump a failure counter and potentially trigger a reboot.
    void noteTelegramFailure();

    IModem &modem_;
    IBotClient &bot_;
    RebootFn reboot_;
    ClockFn clock_;
    ReplyTargetMap *replyTargets_ = nullptr;
    int consecutiveFailures_ = 0;

    // Holds the in-flight concatenated groups. Vector is fine at this
    // scale (cap = 8).
    std::vector<ConcatGroup> concatGroups_;
    size_t totalBufferedBytes_ = 0;
};
