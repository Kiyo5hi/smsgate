#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <functional>
#include <vector>

#include "imodem.h"
#include "ibot_client.h"
#include "sms_codec.h"
#include "sms_block_list.h"

// Forward declarations — sms_handler doesn't otherwise need to know
// about reply target storage or the debug log; it just calls their
// narrow APIs via the pointer.
class ReplyTargetMap;
class SmsDebugLog;

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

    // RFC-0061: Duplicate-suppression window and ring size. Public so tests
    // can reference them without hard-coding magic numbers.
    static constexpr unsigned long kDedupWindowMs = 30000UL; // 30 seconds
    static constexpr size_t kDedupSlots = 8;

    // Clock defaults to `millis()` if not supplied.
    SmsHandler(IModem &modem, IBotClient &bot, RebootFn reboot, ClockFn clock = nullptr);

    // Optional: attach a ReplyTargetMap (RFC-0003 §2). When set, every
    // successful SMS forward writes a (telegram_message_id, sms_sender)
    // entry into the map so a future Telegram reply can route back
    // to the original SMS sender. Pass nullptr (or just don't call
    // this) to disable the bidirectional path.
    void setReplyTargetMap(ReplyTargetMap *map) { replyTargets_ = map; }

    // Optional: attach a diagnostic log. When set, every PDU parse
    // result is pushed into the ring buffer so the user can inspect
    // concat metadata / encoding / outcomes via a Telegram /debug
    // command.
    void setDebugLog(SmsDebugLog *log) { debugLog_ = log; }

    // RFC-0041: Optional callback fired after each successful SMS forward
    // (single-part or assembled concat). Production wires this to a lambda
    // that records time(nullptr) so /status can show the last-received time.
    void setOnForwarded(std::function<void()> cb) { onForwarded_ = std::move(cb); }

    // RFC-0070: Set extra Telegram recipients for forwarded SMS. When set,
    // each successfully forwarded SMS (single-part or assembled concat) is
    // also sent to each extra chat ID via sendMessageTo (no reply-routing).
    // Only the admin (sendMessageReturningId) can reply-route back.
    // The array must remain valid for the lifetime of SmsHandler.
    void setExtraRecipients(const int64_t *ids, int count)
    {
        extraRecipients_ = ids;
        extraRecipientCount_ = count;
    }

    // Optional: set an SMS sender block list (RFC-0018). When set,
    // any incoming SMS whose sender exactly matches an entry in the
    // list is silently deleted from the SIM without forwarding to
    // Telegram. Pass nullptr / 0 (or just don't call this) to disable.
    // The array must remain valid for the lifetime of SmsHandler.
    void setBlockList(const char (*list)[kSmsBlockListMaxNumberLen + 1], int count)
    {
        blockList_ = list;
        blockListCount_ = count;
    }

    // Optional: set a runtime SMS sender block list (RFC-0021). Checked
    // in addition to the compile-time block list. Call from main.cpp after
    // loading the NVS blob, and again after each /block or /unblock command.
    // The array must remain valid for the lifetime of SmsHandler (file-scope
    // static in main.cpp satisfies this).
    void setRuntimeBlockList(const char (*list)[kSmsBlockListMaxNumberLen + 1], int count)
    {
        runtimeList_ = list;
        runtimeListCount_ = count;
    }

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
    // Returns the count of SIM indices dispatched to handleSmsIndex.
    int sweepExistingSms();

    // Test-only accessor — no runtime caller.
    int consecutiveFailures() const { return consecutiveFailures_; }

    // Test-only accessor — returns number of in-flight concat groups.
    size_t concatKeyCount() const { return concatGroups_.size(); }

    // RFC-0069: Human-readable summary of all in-flight concat groups.
    // Returns "(no concat groups in flight)" if none.
    String concatGroupsSummary() const;

    // Lifetime counters (RAM only, reset on reboot).
    // smsForwarded_: messages (or assembled concat groups) successfully
    //                delivered to Telegram.
    // telegramSendFailures_: individual Telegram send attempts that failed
    //                        (includes transient failures; NOT a count of
    //                        permanently lost messages — a single message
    //                        that exhausts retries counts once per attempt).
    // smsBlocked_: SMS silently dropped by the block list (RFC-0018/0021).
    // smsDeduplicated_: SMS suppressed as duplicates within the dedup window
    //                   (RFC-0061).
    int smsForwarded() const { return smsForwarded_; }
    int smsFailed() const { return telegramSendFailures_; }
    int smsBlocked() const { return smsBlocked_; }
    int smsDeduplicated() const { return smsDeduplicated_; }

    // RFC-0110: Reset all session-level counters to zero without affecting
    // NVS-persisted lifetime counters. Safe to call at any time.
    void resetStats()
    {
        smsForwarded_ = 0;
        telegramSendFailures_ = 0;
        smsBlocked_ = 0;
        smsDeduplicated_ = 0;
        consecutiveFailures_ = 0;
    }

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
    //
    // RFC-0061: If `pWasDuplicate` is non-null, it is set to true when
    // the assembled message was suppressed as a duplicate (still returns
    // true so the caller deletes the SIM slots).
    bool insertFragmentAndMaybePost(const sms_codec::SmsPdu &pdu, int simIndex,
                                    std::vector<int> &deleteSlots,
                                    bool *pWasDuplicate = nullptr);

    // Drop expired / overflowed groups. LRU by lastSeenMs.
    void evictExpiredLocked(unsigned long now);
    void evictLruUntilUnderCaps(size_t reservedExtraBytes);

    // Look up a group by (sender, ref). Returns nullptr if missing.
    ConcatGroup *findGroup(const String &sender, uint16_t ref);

    // Bump a failure counter and potentially trigger a reboot.
    void noteTelegramFailure();

    // RFC-0061: djb2 hash of (sender + '\0' + body); never returns 0.
    static uint32_t dedupHash(const String &sender, const String &body);

    // RFC-0061: Return true if (sender, body) was successfully forwarded
    // within kDedupWindowMs (check only, does NOT record).
    bool checkDup(const String &sender, const String &body) const;

    // RFC-0061: Record that (sender, body) was just successfully forwarded.
    // Call this AFTER a successful Telegram send so that failed attempts
    // never populate the ring (retries must still reach the bot).
    void recordDedup(const String &sender, const String &body);

    // RFC-0061: Dedup ring buffer. Each slot stores the djb2 hash and the
    // millis()-since-boot timestamp at which the message was forwarded.
    // Hash=0 is the "empty slot" sentinel (dedupHash guarantees non-zero).
    struct DedupEntry { uint32_t hash = 0; unsigned long tsMs = 0; };
    DedupEntry dedupRing_[kDedupSlots] = {};
    size_t dedupHead_ = 0;

    IModem &modem_;
    IBotClient &bot_;
    RebootFn reboot_;
    ClockFn clock_;
    ReplyTargetMap *replyTargets_ = nullptr;
    SmsDebugLog *debugLog_ = nullptr;
    std::function<void()> onForwarded_;
    const int64_t *extraRecipients_ = nullptr; // RFC-0070
    int extraRecipientCount_ = 0;             // RFC-0070
    const char (*blockList_)[kSmsBlockListMaxNumberLen + 1] = nullptr;
    int blockListCount_ = 0;
    const char (*runtimeList_)[kSmsBlockListMaxNumberLen + 1] = nullptr;
    int runtimeListCount_ = 0;
    int consecutiveFailures_ = 0;
    int smsForwarded_ = 0;
    int telegramSendFailures_ = 0;
    int smsBlocked_ = 0;      // RFC-0062
    int smsDeduplicated_ = 0; // RFC-0062

    // Holds the in-flight concatenated groups. Vector is fine at this
    // scale (cap = 8).
    std::vector<ConcatGroup> concatGroups_;
    size_t totalBufferedBytes_ = 0;
};
