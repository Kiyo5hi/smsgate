#pragma once

#include <Arduino.h>
#include <functional>
#include <array>
#include <vector>

#include "imodem.h"
#include "delivery_report_map.h"

class SmsDebugLog; // forward declaration for RFC-0035 log sink

// Outbound SMS sender. Builds an SMS-SUBMIT PDU via sms_codec and
// sends it through IModem::sendPduSms, which stays in PDU mode —
// no text-mode flip, no post-send restoration needed.
//
// Auto-selects GSM-7 (160 char capacity) when the body is GSM-7
// compatible, falls back to UCS-2 / UTF-16BE (70 chars) otherwise.
// Supports the full BMP plus supplementary Unicode (surrogate pairs).
//
// `lastError` returns a human-readable string for the last failed
// send so the poller can include it in the error reply to the user.
class ISmsSender
{
public:
    virtual ~ISmsSender() = default;

    // Send `body` to phone number `number`. Returns true on success.
    virtual bool send(const String &number, const String &body) = 0;

    // Reason for the most recent failure, or "" if none / last call
    // succeeded. Stable until the next send() call.
    virtual const String &lastError() const = 0;
};

// One pending outbound SMS in the retry queue (RFC-0012).
struct OutboundEntry {
    String phone;
    String body;
    int attempts;           // how many send attempts have been made so far
    uint32_t nextRetryMs;   // drainQueue(nowMs) only acts when nowMs >= this
    uint32_t queuedAtMs;    // RFC-0095: set to nowMs on first drain attempt
    std::function<void()> onFinalFailure; // called once on final drop; may be nullptr
    std::function<void()> onSuccess;      // called once on successful send; may be nullptr
    bool occupied;          // true when this slot is in use
};

class SmsSender : public ISmsSender
{
public:
    // Queue capacity (RFC-0012 §1).
    static constexpr int kQueueSize = 8;
    // Maximum send attempts per entry before dropping.
    static constexpr int kMaxAttempts = 5;
    // Backoff delays (ms) indexed by attempt number (0-based).
    // kBackoffMs[0] = 0 means the first attempt is immediate.
    // kBackoffMs[i] for i >= 1 is the delay after the i-th failure.
    static constexpr uint32_t kBackoffMs[5] = {0, 2000, 4000, 8000, 16000};

    explicit SmsSender(IModem &modem);

    bool send(const String &number, const String &body) override;
    const String &lastError() const override { return lastError_; }

    // Queue `body` for delivery to `phone` with exponential-backoff retry.
    // Returns true if the entry was accepted into the queue.
    // If the queue is full, calls onFinalFailure immediately and returns false.
    // onFinalFailure / onSuccess may be nullptr (no notification).
    // Both the poller and SmsSender are process-lifetime objects so it is
    // safe for the lambdas to capture a raw TelegramPoller* — see
    // RFC-0012 §3 and "Notes for handover".
    bool enqueue(const String &phone, const String &body,
                 std::function<void()> onFinalFailure = nullptr,
                 std::function<void()> onSuccess = nullptr);

    // Attempt to send at most ONE pending queue entry whose nextRetryMs <=
    // nowMs. Returns immediately if the queue is empty or nothing is due.
    // On success the entry is removed. On failure the attempt count is
    // incremented and nextRetryMs is advanced by kBackoffMs[attempts].
    // After kMaxAttempts failures the entry is removed and onFinalFailure
    // is called (if non-null).
    // The caller (loop() in main.cpp) passes (uint32_t)millis(); tests
    // pass a controlled value directly — no clock member on SmsSender.
    // NOTE: do not call drainQueue from inside processUpdate or tick().
    // It must run as a separate step in loop() after the poller tick so
    // AT commands are not issued while the poller HTTP exchange is in flight.
    void drainQueue(uint32_t nowMs);

    // Number of entries currently in the queue. For testing / introspection.
    int queueSize() const;

    // RFC-0091: Session counters — reset on construction, never persisted.
    int sentCount()   const { return sentCount_; }
    int failedCount() const { return failedCount_; }
    // RFC-0110: Reset outbound session counters.
    void resetStats() { sentCount_ = 0; failedCount_ = 0; }

    // Snapshot of occupied queue entries (RFC-0033: /queue command).
    // Each entry carries phone, a body preview (up to 20 chars), and
    // the attempt count (0 = first attempt not yet made).
    struct QueueSnapshot {
        String phone;
        String bodyPreview; // first ≤20 chars of body
        int attempts;
        uint32_t queuedAtMs;   // RFC-0095: set on first drain; 0 if not yet drained
        uint32_t nextRetryMs;  // RFC-0214: millis() when next attempt is allowed; 0 if ready now
        String   bodyFull;     // RFC-0214: complete body text
    };
    std::vector<QueueSnapshot> getQueueSnapshot() const;

    // RFC-0087: Reset all retry timers to zero so the next drainQueue call
    // immediately attempts all pending entries regardless of backoff state.
    void resetRetryTimers();

    // RFC-0046: Cancel the Nth occupied queue entry (1-indexed, matching
    // /queue display order). Returns true if found and removed, false if
    // N is out of range. onFinalFailure is NOT called — cancellation is
    // intentional, not a retry-exhaustion failure.
    bool cancelQueueEntry(int n);

    // RFC-0089: Remove ALL occupied queue entries. onFinalFailure is NOT
    // called for any entry — this is an intentional bulk discard.
    // Returns the number of entries cleared.
    int clearQueue();

    // RFC-0136: Remove all queue entries for the given phone number.
    // Returns the number of entries removed. onFinalFailure is NOT called.
    int cancelByPhone(const String &phone);

    // Attach a DeliveryReportMap so single-part sends store the MR for
    // correlation with +CDS URCs (RFC-0011). Pass nullptr to disable.
    // Lifetime: map must outlive SmsSender. Default: nullptr (disabled).
    void setDeliveryReportMap(DeliveryReportMap *map)
    {
        deliveryReportMap_ = map;
    }

    // Attach a debug log to record outbound SMS failures (RFC-0035):
    // queue-full rejections and final-failure drops are pushed as
    // log entries with outcome "out:queue_full" / "out:fail: <reason>".
    // Pass nullptr to disable. Lifetime: log must outlive SmsSender.
    void setDebugLog(SmsDebugLog *log) { debugLog_ = log; }

    // RFC-0160: Runtime-configurable maximum concat parts (1–10, default 10).
    // Limits how many parts buildSmsSubmitPduMulti will generate, so
    // operators on expensive plans can cap outbound message length.
    void setMaxParts(int n)
    {
        if (n < 1) n = 1;
        if (n > 10) n = 10;
        maxParts_ = n;
    }
    int maxParts() const { return maxParts_; }

private:
    IModem &modem_;
    String lastError_;
    std::array<OutboundEntry, kQueueSize> queue_;
    DeliveryReportMap *deliveryReportMap_ = nullptr;
    SmsDebugLog *debugLog_ = nullptr;
    int sentCount_   = 0; // RFC-0091: session-only, not persisted
    int failedCount_ = 0;
    int maxParts_ = 10;   // RFC-0160: max concat parts (1-10)
};
