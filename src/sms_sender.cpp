#include "sms_sender.h"
#include "sms_codec.h"
#include "sms_debug_log.h"

#ifdef ESP_PLATFORM
#include <esp_task_wdt.h>
#endif

// Out-of-line definition required for constexpr array with external linkage
// on older C++ standards (pre-C++17 inline variables).
constexpr uint32_t SmsSender::kBackoffMs[5];

SmsSender::SmsSender(IModem &modem)
    : modem_(modem), queue_{}
{
    // Mark all queue slots as unoccupied.
    for (auto &e : queue_)
        e.occupied = false;
}

bool SmsSender::send(const String &number, const String &body)
{
    lastError_ = String();

    if (number.length() == 0)
    {
        lastError_ = "destination phone is empty";
        return false;
    }

    if (body.length() == 0)
    {
        lastError_ = "SMS body is empty";
        return false;
    }

    // Build the SMS-SUBMIT PDU(s). Auto-selects GSM-7 (160 char single-
    // part / 153 char per concat part) or UCS-2 (70 / 67 chars) based
    // on body content.  Splits automatically into up to 10 concat parts.
    // Request a delivery status report (TP-SRR bit) when the
    // DeliveryReportMap is wired in — that's the signal that
    // -DENABLE_DELIVERY_REPORTS is active in this build.
    bool requestSR = (deliveryReportMap_ != nullptr);
    auto pdus = sms_codec::buildSmsSubmitPduMulti(number, body, 10, requestSR);
    if (pdus.empty())
    {
        bool gsm7 = sms_codec::isGsm7Compatible(body);
        if (gsm7)
            lastError_ = "SMS too long (max ~1530 chars for GSM-7)";
        else
            lastError_ = "SMS too long (max ~670 chars for Unicode)";
        return false;
    }

    for (size_t i = 0; i < pdus.size(); ++i)
    {
#ifdef ESP_PLATFORM
        esp_task_wdt_reset();  // RFC-0015: each part can block up to 70s
#endif
        Serial.print("SmsSender: sending part ");
        Serial.print((int)(i + 1));
        Serial.print("/");
        Serial.print((int)pdus.size());
        Serial.print(" to ");
        Serial.print(number);
        Serial.print(" tpduLen=");
        Serial.print(pdus[i].tpduLen);
        Serial.print(" hexLen=");
        Serial.println(pdus[i].hex.length());

        int mr = modem_.sendPduSms(pdus[i].hex, pdus[i].tpduLen);
        if (mr < 0)
        {
            lastError_ = "modem rejected part " + String((int)(i + 1))
                       + " of " + String((int)pdus.size());
            Serial.println("SmsSender: send FAILED");
            return false;
        }

        // Store MR for delivery report correlation (RFC-0011).
        // Only track single-part sends — multi-part tracking is deferred.
        if (deliveryReportMap_ != nullptr)
        {
            if (pdus.size() == 1)
            {
                deliveryReportMap_->put(
                    (uint8_t)mr, number, (uint32_t)millis());
            }
            else if (i == 0)
            {
                // Log once at the start of a multi-part send.
                Serial.println(
                    "SmsSender: delivery report tracking skipped for multipart SMS");
            }
        }
    }

    Serial.println("SmsSender: send OK");
    return true;
}

// ---- RFC-0012: outbound queue with exponential-backoff retry ----

bool SmsSender::enqueue(const String &phone, const String &body,
                        std::function<void()> onFinalFailure,
                        std::function<void()> onSuccess)
{
    // RFC-0111: Dedup check — reject if an identical (phone, body) entry
    // is already pending in the queue.
    for (const auto &e : queue_)
    {
        if (e.occupied && e.phone == phone && e.body == body)
        {
            Serial.print("SmsSender: duplicate enqueue rejected for ");
            Serial.println(phone);
            return false;
        }
    }

    // Find a free slot.
    for (auto &e : queue_)
    {
        if (!e.occupied)
        {
            e.phone          = phone;
            e.body           = body;
            e.attempts       = 0;
            e.nextRetryMs    = 0; // first attempt is immediate
            e.onFinalFailure = onFinalFailure;
            e.onSuccess      = onSuccess;
            e.occupied       = true;
            Serial.print("SmsSender: enqueued SMS to ");
            Serial.println(phone);
            return true;
        }
    }

    // Queue is full — reject the new entry (do not evict existing ones).
    Serial.println("SmsSender: outbound queue full, dropping new message");
    if (debugLog_)
    {
        SmsDebugLog::Entry le;
#ifdef ESP_PLATFORM
        le.timestampMs = millis();
#endif
        le.sender    = phone; // repurposed: recipient
        le.bodyChars = (uint16_t)(body.length() > 65535u ? 65535u : body.length());
        le.outcome   = String("out:queue_full");
        le.pduPrefix = body.substring(0, 40); // body preview
        debugLog_->push(le);
    }
    if (onFinalFailure)
        onFinalFailure();
    return false;
}

void SmsSender::drainQueue(uint32_t nowMs)
{
    // Attempt at most ONE send per call so the loop stays non-blocking.
    for (auto &e : queue_)
    {
        if (!e.occupied)
            continue;
        if (nowMs < e.nextRetryMs)
            continue;

        // This entry is due — record first-drain timestamp (RFC-0095).
        if (e.queuedAtMs == 0)
            e.queuedAtMs = nowMs;

        // Attempt to send.
        bool ok = send(e.phone, e.body);
        if (ok)
        {
            Serial.print("SmsSender: queue entry delivered to ");
            Serial.println(e.phone);
            // RFC-0086: Log success to debug log.
            if (debugLog_)
            {
                SmsDebugLog::Entry le;
                le.timestampMs = nowMs;
                le.sender      = e.phone; // repurposed: recipient for outbound
                le.bodyChars   = (uint16_t)(e.body.length() > 65535u ? 65535u : e.body.length());
                le.outcome     = String("out:sent");
                le.pduPrefix   = e.body.substring(0, 40);
                debugLog_->push(le);
            }
            sentCount_++; // RFC-0091
            auto cb = e.onSuccess; // copy before clearing slot
            e.occupied = false;
            if (cb)
                cb();
        }
        else
        {
            e.attempts++;
            Serial.print("SmsSender: queue entry failed (attempt ");
            Serial.print(e.attempts);
            Serial.print("/");
            Serial.print(kMaxAttempts);
            Serial.print(") to ");
            Serial.println(e.phone);

            if (e.attempts >= kMaxAttempts)
            {
                // All retries exhausted — drop the entry.
                Serial.print("SmsSender: queue entry permanently failed to ");
                Serial.println(e.phone);
                if (debugLog_)
                {
                    SmsDebugLog::Entry le;
                    le.timestampMs = nowMs;
                    le.sender      = e.phone; // repurposed: recipient
                    le.bodyChars   = (uint16_t)(e.body.length() > 65535u ? 65535u : e.body.length());
                    le.outcome     = String("out:fail: ") + lastError_;
                    le.pduPrefix   = e.body.substring(0, 40);
                    debugLog_->push(le);
                }
                failedCount_++; // RFC-0091
                auto cb = e.onFinalFailure; // copy before clearing slot
                e.occupied = false;
                if (cb)
                    cb();
            }
            else
            {
                // Schedule next retry with backoff.
                // kBackoffMs is indexed by attempt count (1..4 -> 2s,4s,8s,16s).
                int idx = (e.attempts < 5) ? e.attempts : 4;
                e.nextRetryMs = nowMs + kBackoffMs[idx];
            }
        }
        // Only one send attempt per drainQueue call.
        return;
    }
}

void SmsSender::resetRetryTimers()
{
    for (auto &e : queue_)
        if (e.occupied)
            e.nextRetryMs = 0;
}

int SmsSender::queueSize() const
{
    int count = 0;
    for (const auto &e : queue_)
        if (e.occupied)
            ++count;
    return count;
}

std::vector<SmsSender::QueueSnapshot> SmsSender::getQueueSnapshot() const
{
    std::vector<QueueSnapshot> result;
    for (const auto &e : queue_)
    {
        if (!e.occupied)
            continue;
        QueueSnapshot s;
        s.phone       = e.phone;
        s.bodyPreview = e.body.substring(0, 20);
        s.attempts    = e.attempts;
        s.queuedAtMs  = e.queuedAtMs; // RFC-0095
        result.push_back(s);
    }
    return result;
}

bool SmsSender::cancelQueueEntry(int n)
{
    int count = 0;
    for (auto &e : queue_)
    {
        if (!e.occupied)
            continue;
        if (++count == n)
        {
            e.occupied = false;
            return true;
        }
    }
    return false;
}

int SmsSender::clearQueue()
{
    int cleared = 0;
    for (auto &e : queue_)
    {
        if (e.occupied)
        {
            e.occupied = false;
            cleared++;
        }
    }
    return cleared;
}
