#pragma once

#include <Arduino.h>
#include <vector>

#include "ibot_client.h"

// Scripted fake Telegram client. `sendMessage` returns whatever was
// queued (default: true) and records every message the handler tried
// to send so tests can assert content / ordering.
//
// `sendMessageReturningId` returns a monotonically increasing fake
// message id starting at 1000 (so tests can hard-code expected ids
// without colliding with default ints), or 0 on failure.
//
// `pollUpdates` returns batches of TelegramUpdate scripts queued via
// `queueUpdateBatch`. Each call drains one batch.
class FakeBotClient : public IBotClient
{
public:
    // Queue the return value for the next sendMessage() call. If the
    // queue is empty when sendMessage() is called, `defaultReturn` is
    // used (initialized to true).
    void queueResult(bool ok) { results_.push_back(ok); }

    // Set the return value for all calls with no queued result.
    void setDefault(bool ok) { defaultReturn_ = ok; }

    bool sendMessage(const String &text) override
    {
        sent_.push_back(text);
        if (!results_.empty())
        {
            bool r = results_.front();
            results_.erase(results_.begin());
            return r;
        }
        return defaultReturn_;
    }

    int32_t sendMessageReturningId(const String &text) override
    {
        sent_.push_back(text);
        bool ok;
        if (!results_.empty())
        {
            ok = results_.front();
            results_.erase(results_.begin());
        }
        else
        {
            ok = defaultReturn_;
        }
        if (!ok)
        {
            return 0;
        }
        return ++lastFakeMsgId_;
    }

    // ---- pollUpdates (RFC-0003) ----

    struct PollResult
    {
        bool ok = true;
        std::vector<TelegramUpdate> updates;
    };

    void queueUpdateBatch(std::vector<TelegramUpdate> updates)
    {
        PollResult r;
        r.ok = true;
        r.updates = std::move(updates);
        pollResults_.push_back(std::move(r));
    }

    void queuePollFailure()
    {
        PollResult r;
        r.ok = false;
        pollResults_.push_back(std::move(r));
    }

    bool pollUpdates(int32_t sinceUpdateId, int32_t /*timeoutSec*/,
                     std::vector<TelegramUpdate> &out) override
    {
        pollCallCount_++;
        lastPollOffset_ = sinceUpdateId;
        out.clear();
        if (pollResults_.empty())
        {
            // Default: empty success
            return true;
        }
        PollResult r = std::move(pollResults_.front());
        pollResults_.erase(pollResults_.begin());
        if (!r.ok)
        {
            return false;
        }
        out = std::move(r.updates);
        return true;
    }

    int pollCallCount() const { return pollCallCount_; }
    int32_t lastPollOffset() const { return lastPollOffset_; }

    const std::vector<String> &sentMessages() const { return sent_; }

    size_t callCount() const { return sent_.size(); }

    int32_t lastIssuedMessageId() const { return lastFakeMsgId_; }

private:
    std::vector<bool> results_;
    bool defaultReturn_ = true;
    std::vector<String> sent_;
    int32_t lastFakeMsgId_ = 1000;
    std::vector<PollResult> pollResults_;
    int pollCallCount_ = 0;
    int32_t lastPollOffset_ = -1;
};
