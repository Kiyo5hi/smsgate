#pragma once

#include <Arduino.h>
#include <functional>
#include <array>

#include "imodem.h"
#include "delivery_report_map.h"

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
    std::function<void()> onFinalFailure; // called once on final drop; may be nullptr
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
    // onFinalFailure may be nullptr (no notification on final drop).
    // Both the poller and SmsSender are process-lifetime objects so it is
    // safe for onFinalFailure to capture a raw TelegramPoller* — see
    // RFC-0012 §3 and "Notes for handover".
    bool enqueue(const String &phone, const String &body,
                 std::function<void()> onFinalFailure = nullptr);

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

    // Attach a DeliveryReportMap so single-part sends store the MR for
    // correlation with +CDS URCs (RFC-0011). Pass nullptr to disable.
    // Lifetime: map must outlive SmsSender. Default: nullptr (disabled).
    void setDeliveryReportMap(DeliveryReportMap *map)
    {
        deliveryReportMap_ = map;
    }

private:
    IModem &modem_;
    String lastError_;
    std::array<OutboundEntry, kQueueSize> queue_;
    DeliveryReportMap *deliveryReportMap_ = nullptr;
};
